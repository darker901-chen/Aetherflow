# AetherFlow — Design Decisions

The non-obvious engineering choices in AetherFlow and *why* they were made. Each
entry cites a source location so it can be checked against the code.

---

## DD-1 — A privacy-mask failure never encodes the frame unmasked

**Invariant.** When a privacy-mask apply fails at runtime, the pipeline never
falls back to encoding the *unmasked* frame. The target recovery behavior is to
drop that single frame and keep the capture/encode session running.

**Why (author's words).**

> — *"Even if the mask fails, it must not break the streaming."*

This is a live screen-share privacy guard, so the two tempting failure
modes are both wrong:

- *Abort the session* — a dropped frame is recoverable; a dead stream in the
  middle of a live share is not.
- *Encode the frame without the mask* — that emits exactly the sensitive region
  (password field, chat window) the mask existed to hide. A privacy tool that
  fails open *to the content* is worse than no tool.

Skipping the frame is the target failure mode that keeps **both** guarantees:
nothing sensitive is ever emitted, and the stream survives a transient error.

**Where.** macOS is the reference implementation — on a compose failure the shim
traces `encodeOk=false` and `continue`s to the next frame
(`src/platform/mac/MacosPlatformShim.mm:413-428`). The Windows D3D11 path
currently takes a stricter content-fail-closed action: it latches
`privacyMaskFatal` at `src/main.cpp:1708-1712` and aborts the run at
`src/main.cpp:1908-1911`. Aligning Windows to skip-the-frame behavior — and not
aborting when every mask rectangle is empty, where the original frame is already
safe (`src/PrivacyMaskCompositor.cpp:230`) — remains a pending Windows fix with a
unit test.

---

## DD-2 — The runtime stays deterministic; the AI classifier is advisory and off the fast path

**Decision.** The product runtime (capture → scene decision → mask → encode) is
**local and deterministic**. The on-device scene classifier runs on a **separate
~1 Hz slow path**, is **advisory only** (it annotates the trace; it does not gate
the encoder or the mask), and is **opt-in / default-off**.

**Why.** The capture→encode fast path must never block — a stalling or wrong
inference cannot be allowed to drop frames or leak content. Keeping the model on
a droppable, sampled slow path, decoupled from the fast path by the
`AsyncAnalyzerBridge` seam, means the deterministic guarantees (privacy masks,
latency) hold regardless of whether — or how well — the model runs. The
classifier is also unproven today (CLIP zero-shot, ~60–75 % expected accuracy)
and has no product-visible downstream action yet, so keeping it advisory stops an
unreliable component from affecting a reliable product.

Honest note — the author first leaned the other way:

> — *"I originally wanted it on — detecting by default."*

It stays opt-in until the model is shippable and detection drives a real action.

**Where.** The classifier activates only when a model path is supplied
(`src/main.cpp:929`), is compile-guarded by `AETHERFLOW_ENABLE_SCENE_CLASSIFIER`,
and feeds an advisory `PolicyEngine` that stamps the trace without steering the
encoder.

---

## DD-3 — One scene decision drives every downstream action (scene-first)

**Decision.** The runtime makes **one** scene-type decision per frame; ROI, QP,
and privacy masks are **downstream actions selected from that scene**, not
independent per-feature detectors.

**Why (author's words).**

> — *"Because the scene is what correctly represents the frame's current state."*

A per-feature detector only sees its own narrow signal; the scene is the
whole-frame semantic truth, and the right action (mask vs ROI vs nothing) follows
from it. It also decouples producers from actions: swapping the producer
(cursor → ONNX → OCR) needs no downstream change, and the encoder never has to
know who decided. (`include/AetherFlow/IAIFrameAnalyzer.h` `FramePolicyEngine`;
ARCHITECTURE §6.1.)

---

## DD-4 — The classifier's pixel readback runs on the producer thread — but never blocks it

**Decision.** `SceneClassifierOnnx::SubmitFrame` does its GPU snapshot on the
**producer (capture/decision) thread**, rather than handing the texture to the
worker thread to read back there.

**Why (verified from code).** The source is the WGC-delivered `captureTextureBgra`,
which is only valid at the current frame's decision stage — hand it to a worker and
it may be released/reused before the worker reads it. So the producer thread
snapshots it *while it is valid* (GPU `Blt` → 224×224 ring slot), then does a
**non-blocking `Map` of the *previous* cycle's slot** (no synchronous GPU wait) and
**drops-when-busy**. The only contact with the hot thread is small and
non-blocking — the cost the fast path can't afford (a synchronous GPU readback
stall) never happens. (`src/ai/SceneClassifierOnnx.cpp:178`; texture-lifetime note
`IAIFrameAnalyzer.h:107`.)

---

## DD-5 — Confidence merge uses strict `>`; ties go to the first writer

**Decision.** `FrameDecision::ProposeScene` keeps a proposed scene iff none is set
yet **or** its confidence is **strictly** greater. Equal confidence →
first-writer-wins (= module registration order).

**Why (documented in the code).** Strict `>` makes ties deterministic via
registration order, so equal-confidence deterministic producers (e.g. 1.0 for
explicit user/CLI actions) keep stable ordering; baseline holds a low confidence so
any producer > 0.5 overrides it. `>=` would let a later equal-confidence writer
clobber an earlier one (non-deterministic w.r.t. registration intent); a separate
priority table would re-encode an ordering that registration order already
provides. (`include/AetherFlow/IAIFrameAnalyzer.h:91`.)

---

## DD-6 — Policy hysteresis: a 3-consecutive debounce plus a 5-second switch floor

**Decision.** `PolicyEngine` only switches mode after seeing the **same class 3
consecutive times** (debounce) and **never more than once per 150 frames = 5 s**
(rate-limit). The constant was raised 60 → 150.

**Why (from code).** The classifier is a noisy ~1 Hz signal; with no hysteresis a
borderline verdict flickers the mode (and the visible demo effect) frame to frame.
Two mechanisms do two jobs: 3-consecutive filters transient noise; the 5 s floor
caps the *visible* switch rate. The `mode_switches_le_1_per_5s` verifier gate
encodes that no-flicker requirement. (`include/AetherFlow/policy/PolicyEngine.h:50`;
flicker rationale `IAIFrameAnalyzer.h:80`.)

---

## DD-7 — Mask shaders are compiled eagerly at init, not lazily on first use

**Decision.** The blur/mosaic/grayscale HLSL pipelines are compiled in
`Initialize()` (~48 ms upfront) even if never used, rather than lazily on first use.

**Why (documented in the code).** Lazy compile would pay the ~48 ms HLSL-compile +
state-object cost **on the first frame** that selects a non-blackout mode — a
single-frame stall that blows the 33 ms budget. Eager moves that cost to startup
(one-time, predictable, off the per-frame hot path) — the same "keep unpredictable
cost off the fast path" principle as DD-2 / the off-thread readback. Fail-safe: if
compile fails, the compositor still works in Blackout and falls back per call.
(`src/PrivacyMaskCompositor.cpp:191`.)

---

## DD-8 — SRT live output taps the encoder drain side; SPS/PPS is harvested at enqueue, not at serve

**Decision.** The SRT stage consumes encoded access units through an
`IEncodedFrameSink` called from the encoder's **drain/consumer-side** threads
(NVENC `DrainSlot`, VPL `WriteBitstreamSample`), never the capture thread. The
sink copies into a bounded **drop-oldest** queue (8 MiB / 256 AUs) and a
dedicated worker owns all libavformat/SRT state. Cached SPS/PPS for mid-stream
joins is extracted **when an AU is enqueued**, not when it is served.

**Why.** (a) Network I/O blocks unpredictably (listener accept blocks for
minutes; a stalled viewer blocks writes) — the same "keep unpredictable cost off
the fast path" principle as DD-2/DD-7, so all of it lives on a thread the
33 ms budget never meets, behind a queue that drops oldest instead of growing.
(b) The enqueue-side SPS/PPS placement is a bug fix proven live: NVENC emits
in-band parameter sets only on the **first** IDR, and the serve loop clears the
queue when a viewer connects (fresh-content policy) — so a serve-side cache
missed frame 0 and every real viewer got an undecodable stream (numerically
confirmed twice: `sent == enqueued − 60`). Alternatives considered: flipping
NVENC `repeatSPSPPS` (rejected — changes the canonical file bitstream bytes);
not clearing the queue on connect (rejected — serves stale backlog, adds
latency). (`src/streaming/SrtStreamOutput.cpp` `OnEncodedAccessUnit`,
`include/AetherFlow/streaming/SrtStreamOutput.h:102`,
`.aetherflow/runs/srt_output_v1/diagnosis.json`.)

---

## DD-9 — The Studio UI compiles the CLI's translation units; main() is a guard, not a fork

**Decision.** `AetherFlowStudio.exe` (spec Delta B) is not a second pipeline:
it compiles the SAME source files as `AetherFlow.exe` — including
`src/main.cpp` — with `AETHERFLOW_STUDIO_BUILD` guarding out `main()` only.
The pipeline body was extracted in place as
`AetherFlow::RunPipelineOnce(const PipelineOptions&, PipelineStatus*)`; the
CLI parses argv/env into options, the UI binds the same options to controls.
Compile-time Config.h constants remain the DEFAULTS for any option left at
its zero sentinel.

**Why.** The repo's whole verification story hangs on the canonical headless
run staying byte-stable — a separately-implemented UI pipeline would fork
behavior invisibly (two mask wirings, two SRT paths, two shutdown semantics)
and none of the agent gates would notice, because they only exercise the CLI.
Sharing translation units makes divergence a LINK ERROR instead of a latent
product bug, and it let the canonical `agent_run`/`agent_verify`/benchmark
gates re-certify the refactor directly (900/900 passed post-extraction).
Alternatives considered: UI-as-launcher spawning the CLI (rejected — status
readouts and Panic would need IPC, and the packaged app would be a
console-window juggler); extracting the pipeline into a static library
(deferred — same effect, more CMake churn; the in-place function keeps the
diff reviewable). (`include/AetherFlow/app/PipelineRunner.h`,
`CMakeLists.txt` Studio target, `src/main.cpp` `AETHERFLOW_STUDIO_BUILD`
guard; evidence `.aetherflow/runs/srt_ui_v1/`.)

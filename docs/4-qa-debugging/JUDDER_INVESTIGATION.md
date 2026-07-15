# Recorded-Video Judder Investigation (2026-05-16 → resolved 2026-05-17)

> A debugging war story: a badly juddery recorded screen-share, root-caused to
> **three compounding bugs (none AI-related)** and fixed. §0 is the resolution;
> §1–§2 are the investigation trail kept for context, and §5 is the one open
> follow-up lead.

---

## 0. RESOLVED (2026-05-17) — read this first

The judder is **fixed**. Root cause was **three compounding bugs, none AI**:

1. **NVENC drain busy-spin + bitstream `ofstream::write` under the pipeline
   `m_stateMutex`** → periodic ~240ms global pipeline freeze. Fixed: drain
   backoff (no hot-spin) + a **dedicated writer thread**; disk I/O never holds
   a pipeline lock. (`src/NvencH264Wrapper.{h,cpp}`)
2. **Detection on the realtime hot path (the dominant residual judder).**
   `PasswordFieldPrivacyMaskModule` (UIAutomation subtree walk) and
   `NotificationProducerModule` (`EnumWindows` + per-process queries) ran
   **synchronously inside `FramePolicyEngine::Evaluate()` on the producer/
   capture thread**, throttled to every 5 frames — every 5th frame cost
   ~27ms of the 33ms budget → producer missed pacing → WGC 4-deep pool
   overflow → dropped capture frames. Fixed: each module now runs its scan on
   a **dedicated background poll thread**; `Evaluate()` only copies a cached
   snapshot (microseconds). Verified: `decisionMs` max 0.30ms / 0% ≥10ms with
   both masks ON (was 19.1% every-5-frame ~27ms).
3. **Fake fixed-30fps mux.** `ffmpeg -r 30 -c copy` baked a uniform timeline
   onto non-uniform capture. Fixed (PD2a/PD3, opt-in): real WGC
   `SystemRelativeTime` → encoder PTS sidecar → `run_scene_test.sh` muxes at
   real timing (mkvmerge per-frame, or ffmpeg real-average-fps fallback).
   Canonical verify path byte-stable (flag unset).

Evidence: interactive run 2026-05-17 20:08, `scene_test_out/` —
`decisionMs` p99 0.17ms, `totalMs` 1.6% over budget (was ~20%),
`demo.mp4` duration 38.31s ≈ real span (not 30s). Residual `captureDeltaMs`
gaps are genuine WGC capture-side variance (PD4: timestamps record it
faithfully, do not invent frames), not a pipeline defect.

The §1–§2 narrative below is the historical investigation trail (kept for
context). The real-timed PTS-sidecar / mux work (PD2a/PD3) is **implemented**
(see §0).

---

## 1. TL;DR — what the judder IS and ISN'T (corrected)

User reports the recorded `scene_test_out/demo.mp4` is badly juddery.

**Ruled OUT (with evidence):**

- ❌ **AI / scene classifier** — `--no-ai` trace: `sceneSource` 900/900 `baseline`, `sceneClass` never emitted. Confirmed not running.
- ❌ **P0.1 regression** — pristine HEAD (`be5ae33`, zero classifier code) compared in a throwaway worktree: HEAD was **equal-or-spikier** (156/900 over-budget vs current 5/900). P0.1 did not regress the bare pipeline.
- ❌ **Capture starvation** — HYPOTHESIZED then **DISPROVEN**. An agent's *headless* run showed 0.48 fps, but the user's **interactive** run shows a **healthy 30.00 fps** (`mean 33.33ms, p50 33.33ms, 0 duplicate frames, WGC SystemRelativeTime=900, QPC fallback=0`). Starvation also can't cause "judder" anyway — it causes slow/frozen, not jitter. **The earlier "starved capture" conclusion was wrong; do not repeat it.**
- ❌ **Fixed-30fps mux mismatch** — since interactive capture IS uniform ~33.33ms/30fps, muxing at `-r 30` is actually *correct* for that run, so it is NOT the cause of the remaining judder.

**Prime suspect NOW (the serious problem the user found): the NVENC encoder / producer-consumer pipeline.**

---

## 2. The actual open problem — encoder pipeline pathology

From the user's interactive run NVENC stats (capture was a healthy 30fps, yet video still juddered):

| Signal | Observed | Interpretation |
|---|---|---|
| `lockBusyCount` | **3,262,752** over 900 frames (~3,600/frame) | something is busy-spinning on a lock every frame |
| `maxInflightSlotsObserved` | **1** | encoder pool only ever uses 1 slot → **pipeline fully serialized, no parallelism** (each frame waits for the previous encode) |
| `[NVENC][LAT] submitAge` | avg ~10.5ms, **max 240.67ms** | periodic large submit stalls |
| `[NVENC][LAT] outputAge` | avg ~11.7ms, **max 244.05ms** | periodic large output stalls |
| `encodedFrameCount` vs captured | **901 vs 900** | off-by-one (extra/duplicate encoded frame) |

**Hypothesis:** capture is steady 30fps, but the encode path is serialized + spins a lock millions of times + periodically stalls ~240ms → the timeline of frames reaching the container is uneven → judder. This is independent of AI, capture, and the mux rate.

`mapFailures=0` — NOT the issue (user explicitly said ignore the map line).

---

## 5. Open follow-up — the encoder serialization lead

Investigate the NVENC encoder + producer-consumer pipeline lock. Specific questions:

1. Why is `maxInflightSlotsObserved=1`? The encoder texture pool has multiple slots — why is the pipeline serialized instead of pipelined (capture N+1 while encoding N)?
2. Why is `lockBusyCount` > 3.2M (~3,600/frame)? Which lock is being hammered/spun (producer-consumer queue? NV12 texture pool? NVENC submit/drain lock?).
3. Source of the periodic ~240ms `submitAge`/`outputAge` spikes.
4. The `encodedFrameCount=901` vs `900` off-by-one (flush emitting an extra frame?).

Owning role: **encoder** agent (NVENC wrapper + drain thread + texture pool), likely with **debug-verifier** for the lock-contention root-cause. Run the **Architecture Planning Gate** before patching (cross-component: pipeline + encoder). Use a real interactive-capture run for evidence, NOT a headless agent run (headless misrepresents capture, as this whole investigation proved).

Key files to start from: `src/NvencH264Wrapper.cpp` (+ header), the producer-consumer pipeline + texture pool in `src/main.cpp`, the encoder drain thread. `[NVENC][STATS]`/`[NVENC][LAT]` emission shows where the counters live.

---

## 7. Hard-won lessons (don't repeat)

- **Never diagnose capture/judder from a headless `agent_run.py` run** — headless WGC starves to ~0.5fps and is NOT representative. Use an interactive run with live moving content; read the always-on `[CAPTURE]` diagnostic line.
- The `[CAPTURE]` effective-fps diagnostic is permanent and the fastest way to classify capture health on any run — use it first.
- Keep the timestamp diagnostic; it is the measurement instrument, not the fix.
- **QA anti-pattern (do not repeat): never run blocking / cross-process
  detection synchronously on the realtime capture→encode (producer) thread,
  even if throttled every N frames.** UIAutomation tree walks, `EnumWindows`
  + per-process image-name queries, registry, disk, network, or GPU readback
  on the hot thread silently starve capture (WGC pool overflow → dropped
  frames → judder). Throttling only changes *how often* the stall hits, never
  the per-hit cost. Offload to a dedicated background poll thread; the
  realtime thread reads only a mutex-protected cached snapshot (microseconds).
  Same class of bug: disk `ofstream::write` under the pipeline `m_stateMutex`
  (use a dedicated writer thread instead). When diagnosing media judder,
  measure per-stage timing and localize the stall to a stage *before*
  changing code — "looks fixed" is not evidence; read the trace.

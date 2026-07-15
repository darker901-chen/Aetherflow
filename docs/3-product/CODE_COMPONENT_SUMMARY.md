# AetherFlow Code Component Summary

This document is a contributor-oriented map of the current C++ codebase.

It is not a replacement for source code. Before changing behavior, always inspect the actual `.cpp` / `.h` files, run the agent workflow, and verify with artifacts.

Consolidation note: this file is a walkthrough, not the canonical owner map.
Use `protocol/COMPONENT_INDEX.md` to locate implementation owners,
`docs/3-product/ARCHITECTURE.md` for durable design facts, and
`docs/1-status/PROJECT_STATUS.md` for current capability/release boundaries.
Dated run IDs and measurements live in
`docs/4-qa-debugging/VERIFICATION_HISTORY.md`.

## Product Context

AetherFlow is moving toward this product shape:

```text
Live Share Guard is the first product wedge.
Scene detection remains the runtime decision core.
Privacy masks are the current product action. ROI, mode switching, and
diagnostics are downstream or deferred actions.
```

The current code already has the right internal vocabulary for this direction:

- `FrameSceneType::SensitiveSurface`
- `FrameRegionPurpose::PrivacyMask`
- `FrameDecision::privacyMasks`
- `IAIFrameAnalyzer` for the current async ONNX scene classifier and future
  OCR / sensitive-region analysis

v0.1 Live Share Guard fast paths are landed: `PrivacyMaskCompositor` applies
emitted masks before encoder submit using D3D11 ClearView blackout or shader
blur/mosaic modes. Four deterministic producers feed the compositor:
`PanicPrivacyMaskModule` (CLI/env startup fullscreen panic; `RightCtrlPanicMaskHotkey` is a dev/test affordance for interactive triggering, not a product feature),
`ManualPrivacyMaskModule` (CLI/env rectangles),
`PasswordFieldPrivacyMaskModule` (UIAutomation foreground `Edit` controls with
`IsPassword=true`, `BoundingRectangle` -> decision space, no AI/OCR), and
`NotificationProducerModule` (visible whitelisted top-level/popup process
window with z-order clipping, no AI/OCR). Verification evidence: see
[PROJECT_STATUS.md](../1-status/PROJECT_STATUS.md) for the current claim
boundary and [VERIFICATION_HISTORY.md](../4-qa-debugging/VERIFICATION_HISTORY.md)
for dated runs and measurements.
Remaining Live Share Guard scope is OCR / QR slow-path sub-region detection,
live messenger notification artifacts, and canonical blur/mosaic visual
artifacts. Product sequencing lives in [PRODUCT_ROADMAP.md](PRODUCT_ROADMAP.md);
current verification boundaries live in
[PROJECT_STATUS.md](../1-status/PROJECT_STATUS.md); dated evidence lives in
[VERIFICATION_HISTORY.md](../4-qa-debugging/VERIFICATION_HISTORY.md).

## Runtime Data Flow

Current high-level flow:

```text
main.cpp
  -> choose encoder backend
  -> initialize ScreenCapture
  -> capture BGRA texture
  -> build FrameContext
  -> FramePolicyEngine evaluates scene / ROI / mask decisions
  -> PrivacyMaskCompositor applies emitted masks to BGRA
  -> convert the masked BGRA texture to NV12
  -> EncodeFrameRequest carries NV12 texture + FrameDecision
  -> IH264Encoder backend encodes H.264
  -> frame_trace.jsonl records timing and decision fields
```

Important detail:

```text
AI / scene inference should be slow path.
Capture, frame processing, and encode must stay deterministic fast path.
```

## Core Files

### `src/main.cpp`

Role: runtime entry point and pipeline scheduler.

Main responsibilities:

- detects Intel / NVIDIA adapters
- chooses NVENC when available, otherwise falls back to Intel oneVPL
- initializes screen capture
- converts captured BGRA frames to NV12 using `D3D11VideoConverter`
- owns producer / consumer pipeline threads
- creates `FrameContext`
- runs `FramePolicyEngine`
- submits `EncodeFrameRequest` to the selected encoder
- writes per-frame trace JSON
- prints latency statistics and per-frame headroom (the stdout field is `SubmitHeadroom` = fast-path / submit-stage headroom; renamed 2026-06-03 from the retired inline-AI label `AIBudget` — AI runs off-thread ~1 Hz, see ARCHITECTURE §3.5)

Important local types:

- `D3D11VideoConverter`: GPU BGRA -> NV12 conversion through `VideoProcessorBlt`
- `BoundedQueue<T>`: fixed-depth producer / consumer queue
- `NV12TexturePool`: reusable NV12 GPU texture pool
- `PipelineFrame`: one frame's texture pointers, timing stamps, and `FrameDecision`

Current decision modules registered in `main.cpp` (registration order =
evaluation order):

- `PanicPrivacyMaskModule`: CLI/env startup fullscreen panic
  (`--panic-mask` / `AETHERFLOW_PRIVACY_PANIC_MASK=1`). `RightCtrlPanicMaskHotkey`
  in `src/main.cpp` is a dev/test affordance for interactive triggering of the
  same module — not a product feature.
- `ManualPrivacyMaskModule`: CLI/env opt-in rectangle producer
- `PasswordFieldPrivacyMaskModule`: deterministic UIAutomation producer for
  foreground `Edit` controls with `IsPassword=true` (default ON; disable with
  `--no-password-field-mask` or `AETHERFLOW_PASSWORD_FIELD_MASK=0`)
- `NotificationProducerModule`: deterministic visible-window producer for
  whitelisted executable leaf filenames, with Teams aliases for packaged
  `ms-teams.exe` / `MSTeams_*` identity (default ON with the bundled whitelist;
  disable with `--no-notification-mask` or `AETHERFLOW_NOTIFICATION_MASK=0`;
  customize with `--notification-mask-process=...` or
  `AETHERFLOW_NOTIFICATION_PROCESS_LIST`)
- `BaselineSceneModule`: emits `GenericScreen` when no scene was set
- `CursorFocusModule`: emits cursor-centered `QualityRoi` (opt-in, default OFF; registered only with `--cursor-roi` / `AETHERFLOW_CURSOR_ROI=1` or static `--roi-x/--roi-y`)
- `AsyncAnalyzerBridgeModule`: registered when an analyzer is active and merges
  its latest non-blocking result into the frame decision
- `PolicyEngine`: registered when the ONNX classifier is active; consumes the
  post-merge scene and writes advisory policy/trace state without steering the
  encoder
- `SceneDemoActionModule`: registered last only when both the classifier and
  demo action are active; maps the stable scene class to an opt-in visual effect

Live Share Guard impact:

- the deterministic mask stage between decision and encode is implemented in
  `PrivacyMaskCompositor` (D3D11 BGRA ClearView blackout plus shader blur and
  mosaic modes); main.cpp invokes it before the BGRA -> NV12 convert when any
  mask is emitted
- `FrameDecision.privacyMasks` is both traced and rendered into the frame
- residual scope: OCR / QR slow-path sub-region detectors and canonical
  live messenger / blur / mosaic artifacts (see current boundaries in
  [PROJECT_STATUS.md](../1-status/PROJECT_STATUS.md) and dated runs in
  [VERIFICATION_HISTORY.md](../4-qa-debugging/VERIFICATION_HISTORY.md))

### `include/AetherFlow/IAIFrameAnalyzer.h`

Role: scene-first runtime decision model.

Key types:

- `FrameSceneType`: semantic scene label such as `TextUi`, `VideoContent`, `SensitiveSurface`
- `FrameRegionPurpose`: currently `QualityRoi` or `PrivacyMask`
- `FrameScene`: scene type, confidence, source, debug label
- `FrameRegion`: rectangle plus purpose, confidence, source, debug label
- `FrameDecision`: the final per-frame decision consumed by downstream stages
- `FrameContext`: per-frame input to deterministic decision modules
- `IFrameDecisionModule`: synchronous deterministic module interface
- `IAIFrameAnalyzer`: async analyzer interface used by the current ONNX
  classifier and reserved for future OCR / sensitive detection

Current implementation:

- `BaselineSceneModule` creates a default `GenericScreen` when no scene was set
- `CursorFocusModule` creates a cursor-centered quality ROI
- `PanicPrivacyMaskModule` emits a fullscreen `SensitiveSurface` + privacy mask
  whenever the panic flag is active (set by CLI `--panic-mask` /
  `AETHERFLOW_PRIVACY_PANIC_MASK=1`, or by the dev/test
  `RightCtrlPanicMaskHotkey` interactive trigger)
- `ManualPrivacyMaskModule` emits CLI/env-supplied privacy mask rectangles
- `PasswordFieldPrivacyMaskModule` is a deterministic UIAutomation detector that
  emits privacy masks for foreground `Edit` controls with `IsPassword=true`
- `NotificationProducerModule` emits privacy masks for visible top-level/popup
  windows whose process executable leaf filename is on the configured whitelist,
  with Teams packaged-app identity aliases, clipped against higher z-order
  windows
- `NullAIFrameAnalyzer` is a no-op placeholder for the default wiring
- `MockSlowAnalyzer` is the first real-thread `IAIFrameAnalyzer` implementation
  (dedicated `std::thread` worker, mutex + condvar, configurable inference
  delay via `AETHERFLOW_MOCK_ANALYZER_INFERENCE_MS`); used together with
  `AsyncAnalyzerBridgeModule` to verify the bridge under realistic
  worker-thread conditions
- `SceneClassifierOnnx` (Phase 4 P0.1, landed and verified on real captured
  pixels 2026-05-15) is the first *real* `IAIFrameAnalyzer`: a CLIP
  zero-shot 5-class classifier on ONNX Runtime + DirectML EP (CPU fallback,
  real-thread worker). `SubmitFrame` first GPU-downscales the captured frame to
  224×224 (`VideoProcessorBlt`), then reads back only ~196 KB via a non-blocking
  3-slot staging ring (`Map(DO_NOT_WAIT)` of the previous cycle's slot, skipped
  on `WAS_STILL_DRAWING`) — real screen pixels, not synthetic input, and the
  producer thread never blocks on the GPU; opt-in via
  `--scene-classifier-onnx-model=` /
  `AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL`; classifier-inactive build is
  byte-equivalent. Model `models/scene_classifier_v1.onnx` is gitignored,
  recreated via `tools/export_clip_zeroshot.py`
- `PolicyEngine` (Phase 4 P0.1, landed 2026-05-15) is the cross-platform
  pure-C++ policy layer that maps the post-merge `FrameScene` into a
  `PolicyDecision { mask_mode, encode_hint, mode_label, reason }` with a
  150-frame / 5-second-at-30-fps switch floor plus 3-consecutive hysteresis,
  `confidence < 0.6` low-confidence
  fallback, and a panic override that resets hysteresis. **Advisory only
  for P0.1**: product consumers are trace and Studio status; the opt-in
  `SceneDemoActionModule` also reads the stable class for a non-product
  compositor preview. Policy does not gate the merged scene or the encoder
  (encoder wiring is P1.1)
- `SceneDemoActionModule` is an opt-in, default-off preview that runs after
  `PolicyEngine`, yields to deterministic privacy masks, and maps the stable
  class to blackout/mosaic/blur/grayscale or passthrough

Important contract:

```text
Scene is the primary signal.
Regions are downstream actions chosen from the scene.
```

This is why `SensitiveSurface` should lead to `privacyMasks`, not to ROI.

### `include/AetherFlow/IH264Encoder.h`

Role: common encoder interface.

Key type:

- `EncodeFrameRequest`

`EncodeFrameRequest` carries:

- NV12 texture
- elapsed timestamp
- full `FrameDecision`
- fallback ROI center

Important method:

- `EncodeFrame(const EncodeFrameRequest&)`

Current default behavior:

- if `decision.qualityRegions` exists, the first ROI center is used
- otherwise fallback ROI coordinates are used
- the default implementation calls the older `EncodeFromYUVWithROI(...)`

Streaming tap (2026-07-03): `SetEncodedFrameSink(AetherFlow::IEncodedFrameSink*)`
is a default no-op virtual. A backend that supports live output calls
`sink->OnEncodedAccessUnit(...)` from its drain/writer-side thread for every
encoded access unit (see `include/AetherFlow/IEncodedFrameSink.h` — the sink
must copy and return, never block). Backends without an override are
unaffected; macOS `IPlatformEncoderMac` is a separate boundary and untouched.

Live Share Guard impact:

- the encoder interface carries `privacyMasks` end-to-end via `EncodeFrameRequest`
- masking is applied before encoder submission by `PrivacyMaskCompositor` on the
  capture-side BGRA texture, not inside rate-control ROI logic; encoder backends
  receive an already-masked NV12 surface and stay focused on rate control / ROI
- this keeps privacy correctness independent of backend (NVENC, VPL) and rate
  control mode (CBR / VBR)

### `include/AetherFlow/app/PipelineRunner.h`, `src/ui/`

Role: the two product front-ends over one pipeline (spec Delta B, 2026-07-03).

- `app/PipelineRunner.h` — `PipelineOptions` (every runtime knob: geometry/
  fps/bitrate/monitor/encoder preference/mask toggles/SRT/panic latch/external
  stop; zero-sentinel fields fall back to the compile-time Config.h defaults),
  `PipelineStatus` (UI-readable atomics, incl. the AI scene readouts:
  classifier state 0/1-DirectML/2-CPU/3-failed, canonical class index +
  confidence, and merge-source kind 1-classifier/2-deterministic-override/
  3-baseline-fallback; the canonical class order is the shared
  `kSceneClassNames` table, cross-referenced with SceneClassifierOnnx.h), and
  `RunPipelineOnce(options, status)` — main()'s former Windows body as a
  callable session. `AetherFlow.exe` = argv/env parse → options → one run;
  `AetherFlowStudio.exe` compiles the SAME translation units
  (`AETHERFLOW_STUDIO_BUILD` guards out `main()` only — see DESIGN_DECISIONS
  DD-9) and drives the function on a worker thread.
- `src/ui/StudioMain.cpp` — the Dear ImGui (Win32+D3D11) settings window:
  spec-table controls, Start/Stop (join only on the worker's completion flag —
  never on `!running`, which races thread start), Panic latch, live status,
  viewer-URL copy, `--ui-smoke` (render-only) and `--ui-selftest`
  (programmatic Start → 60-frame session → clean-stop assertion) gates.
  AI scene detection (2026-07-03): launch-time model auto-detect
  (`AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL` env is authoritative when set,
  else `<exe>/models/` then dev-tree `models/`), an advisory-only settings
  toggle that grays out when no model is found (INI value preserved), a
  per-class colored-dot scene indicator in Status (gray "no confident verdict"
  during baseline/low-confidence fallback; `[deterministic override]` tag when
  a 1.0 producer out-ranks the AI), and a selftest AI leg (150 frames,
  asserts classifier init + ≥1 merge win; skipped when no model on disk).
  Recordings default to `<exe>/output` via `_wputenv_s` (the Win32-only
  `SetEnvironmentVariableW` does not reach the CRT `getenv` the pipeline
  reads — found by the selftest trace path).
- `src/ui/AppConfig.{h,cpp}` — persisted settings: hand-rolled INI beside the
  exe, `Normalize()` clamps every field (fps whitelist, bitrate 500–50000,
  port, libsrt 10–79 passphrase rule); `ai_scene_detection` bool (default
  OFF — the AI is opt-in product-wide). Pure logic; ctest `AppConfigIni`.

### `include/AetherFlow/IEncodedFrameSink.h`, `include/AetherFlow/streaming/`, `src/streaming/`

Role: SRT/MPEG-TS live output stage (spec Delta A, landed 2026-07-03; default OFF).

- `IEncodedFrameSink.h` — the seam: one encoded H.264 access unit
  (`data/size/pts90k/keyframe`) delivered on the encoder's drain-side thread.
  Contract: implementations copy and return; the pointer is dead after the call.
- `streaming/AnnexB.{h,cpp}` — pure-logic Annex B NAL scanner plus SPS/PPS
  detect/extract, used for mid-stream join (a viewer can only start decoding at
  a keyframe that carries parameter sets). No FFmpeg dependency; unit-tested by
  `tests/test_annexb.cpp` (ctest `AnnexBScan`), so it builds in CI without the SDK.
- `streaming/SrtStreamOutput.{h,cpp}` — the sink implementation:
  `OnEncodedAccessUnit` copies into a bounded **drop-oldest** queue
  (8 MiB / 256 AUs) and harvests SPS/PPS from keyframe AUs at enqueue time
  (survives the connect-time queue clear — this placement was an in-task bug
  fix, see run `srt_output_v1` diagnosis); a dedicated worker thread owns all
  libavformat state — `srt://0.0.0.0:<port>` listener (interruptible blocking
  accept), mpegts mux, wait-for-keyframe + SPS/PPS prepend, pts rebase,
  viewer-disconnect → re-listen loop, `[SRT]` console stats. Windows share-URL
  helper (`PrintShareUrls`) lives here so main.cpp needs no winsock includes.

Wiring: `src/main.cpp` parses `--srt-output` / `--srt-port=` /
`--srt-latency-ms=` / `--srt-passphrase=` (env `AETHERFLOW_SRT_*`), constructs
the sink before the encoder (destruction order: encoder first), attaches via
`SetEncodedFrameSink`, prints viewer URLs, and emits a run-end
`[SRT] summary:` line. Compiled only when `third_party/ffmpeg` (vendored LGPL
shared SDK, `tools/fetch_ffmpeg.py`) is present ⇒ `AETHERFLOW_ENABLE_SRT_OUTPUT`.

### `include/AetherFlow/ScreenCapture.h` and `src/ScreenCapture.cpp`

Role: Windows screen capture adapter.

Main responsibilities:

- initializes Windows Graphics Capture
- creates a WGC frame pool
- captures monitor or desktop frames as BGRA D3D11 textures
- provides an event-driven capture path through `FrameArrived`
- falls back to DXGI desktop duplication when WGC setup fails
- tracks capture source geometry for cursor-coordinate mapping

Important methods:

- `Init(...)`
- `CaptureTexture()`
- `TryCaptureTexture()`
- `GetFrameEvent()`
- `GetCaptureWidth()`, `GetCaptureHeight()`, `GetCaptureLeft()`, `GetCaptureTop()`

Cross-platform meaning:

- this file is the Windows capture adapter
- macOS already provides a ScreenCaptureKit adapter in
  `src/platform/mac/MacosScreenCapture.mm`
- future Android should provide a MediaProjection adapter
- the core decision model should not depend directly on WGC

### `include/AetherFlow/NvencH264Wrapper.h` and `src/NvencH264Wrapper.cpp`

Role: NVIDIA NVENC encoder backend.

Main responsibilities:

- creates NVIDIA D3D11 device
- loads NVENC runtime API
- opens encoder session
- registers reusable D3D11 input textures
- exposes encoder-owned input texture pool
- builds QP delta map around ROI
- submits frames to NVENC
- drains encoded frames on a background thread
- records frame telemetry and drop reasons

Important methods:

- `Initialize(...)`
- `AcquireInputTexture(...)`
- `ReleaseInputTexture(...)`
- `EncodeFromYUVWithROI(...)`
- `BuildQpDeltaMap(...)`
- `Flush()`

Important concept:

```text
NVENC path can own registered input textures.
main.cpp copies NV12 into those textures before encode.
```

Live Share Guard impact:

- privacy mask is applied to the capture-side BGRA texture before the NV12
  convert, so NVENC sees an already-masked input
- ROI/QP map stays separate from privacy masking
- mask cost is recorded as `maskMs` in the trace, alongside
  `privacyMaskAppliedCount`, `privacyMaskPath`, and `privacyMaskFallbackUsed`

### `include/AetherFlow/VplH264Wrapper.h` and `src/VplH264Wrapper.cpp`

Role: Intel oneVPL / Quick Sync encoder backend.

Main responsibilities:

- initializes oneVPL session
- creates D3D11 hardware device
- uses D3D11 video-memory surfaces
- configures H.264 CBR encoding
- configures VPL ROI extension
- writes output through Media Foundation sink writer
- drains async pending bitstreams
- uses a D3D11 query fence before video encode reads copied GPU data

Important methods:

- `Initialize(...)`
- `EncodeFromYUVWithROI(...)`
- `MergeYUVtoNV12_GPU(...)`
- `DrainOnePending(...)`
- `DrainReadyPending(...)`
- `WriteBitstreamSample(...)`
- `Flush()`

Important concept:

```text
VPL path keeps frames in GPU video memory.
Copy/fence correctness matters because D3D11 graphics work and video encode work can run on different engines.
```

Live Share Guard impact:

- mask processing is completed on the BGRA texture before the VPL NV12 convert,
  so VPL never reads an unmasked surface
- blur and mosaic already add shader work before the NV12 convert; the existing
  GPU fence pattern remains relevant because ordered D3D11 graphics work must
  complete before the video engine consumes the converted surface

### `include/AetherFlow/Config.h`

Role: compile-time runtime configuration.

Important settings:

- width / height
- FPS
- bitrate
- GOP duration
- max frames
- ROI radius
- ROI delta QP
- static ROI mode
- debug FPS display

Current implication:

- `Config.h` owns compile-time defaults, while `src/main.cpp` and Studio expose
  the supported runtime configuration surfaces
- product-facing options such as mask mode, startup panic mask, password-field
  masking, notification/window masking, encoder selection, bitrate, and SRT
  are runtime controls; `RightCtrlPanicMaskHotkey` remains a dev/test
  affordance, not a product feature

### `include/AetherFlow/NvencRoiDefaults.h` and `include/AetherFlow/VplDefaults.h`

Role: backend-specific ROI constants derived from `Config.h`.

Important settings:

- default ROI radius
- min / max ROI radius
- default ROI delta QP
- min / max delta QP
- background delta QP

Current implication:

- ROI behavior is normalized across NVENC and VPL as much as possible
- this should remain an optional optimization module, not the Live Share Guard product headline

### `include/AetherFlow/ROIGenerator.h` and `src/ROIGenerator.cpp`

Role: older mouse-centered QP map generator.

Main responsibilities:

- creates a macroblock grid for H.264
- reads mouse position
- builds a distance-based QP map
- prints ROI debug information

Current status:

- useful historical / experimental ROI code
- current pipeline mainly uses structured `FrameDecision.qualityRegions`
- should not be the main product direction

## Agent Tools

### `tools/agent_run.py`

Role: canonical build / smoke run entry point.

Expected use:

```powershell
python tools/agent_run.py --run-id <run_id>
```

It creates `.aetherflow/runs/<run_id>` artifacts for later summarization and verification.

### `tools/agent_summarize.py`

Role: summarize an existing run artifact directory.

Expected use:

```powershell
python tools/agent_summarize.py --run-dir .aetherflow/runs/<run_id>
```

### `tools/agent_verify.py`

Role: verify build / run / trace artifacts, optionally including benchmark gates.

Expected use:

```powershell
python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id>
python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id> --run-benchmark
```

## Current Architecture Strengths

- clear split between development agent layer and deterministic runtime layer
- GPU-resident capture / conversion / encode path
- encoder abstraction for NVENC and oneVPL
- structured `FrameDecision` type already supports scene, privacy masks, and
  optional quality regions
- trace JSON records scene, privacy mask count/path/timing, and optional ROI
  count for engineering benchmarks
- producer / consumer pipeline helps capture and encode overlap
- no runtime LLM dependency

## Current Gaps

- `IAIFrameAnalyzer` is wired into the policy engine via
  `AsyncAnalyzerBridgeModule` (P2 + Bridge Hardening 2026-05-12). The first
  real-thread analyzer client was `MockSlowAnalyzer` (`std::thread` worker,
  configurable inference delay, drop-when-busy) used to verify the bridge
  contract end-to-end without a model. **The first *real* analyzer landed
  and was verified on real captured pixels on 2026-05-15: `SceneClassifierOnnx`
  (Phase 4 P0.1 Windows, CLIP zero-shot, ONNX Runtime + DirectML EP).**
  macOS (CoreML, P0.2) and Android (LiteRT, P1.0) analyzers are still
  pending. OCR / QR slow-path sub-region detectors are not built and are
  explicitly out of Phase 4 scope
- compositor supports blackout, blur, mosaic, and internal scene-demo
  grayscale; Windows product mode defaults to blur and uses blackout as the
  fail-closed fallback (macOS defaults to mosaic). Grayscale is not a Windows
  CLI/env product-mode value
- scene merging is confidence-based via `FrameDecision::ProposeScene` (strict
  `>` tie-breaker; P1 closed 2026-05-12). **Policy-layer** hysteresis landed
  with P0.1 (`PolicyEngine`, advisory only — trace/Studio plus an opt-in
  non-product demo consumer; it does not gate the merged scene or encoder).
  Still open: M1 calibrated-confidence semantics and
  the **merge-layer** `SceneStabilizerModule` (M2) in
  [ARCHITECTURE.md](ARCHITECTURE.md#13-known-gaps-and-next-steps) §13.1 — those are
  separate cleanup tasks, not P0.1 blockers
- platform interfaces still leak D3D11 types, which blocks clean macOS / Android
  adapters later
- `Config.h` still holds legacy compile-time defaults, while product toggles
  (mask producers/mode, panic, AI, SRT, ROI) are runtime CLI/env or Studio
  settings owned primarily by `src/main.cpp`
- encoder backends still consume `qualityRegions.front()` only; this is a
  deferred encoder optimization, not the Live Share Guard product path

## Live Share Guard Status Pointers

This walkthrough does not keep a second copy of the v0.1 status table.

- Current pass/fail boundaries: [PROJECT_STATUS.md](../1-status/PROJECT_STATUS.md)
- Dated run IDs and measurements: [VERIFICATION_HISTORY.md](../4-qa-debugging/VERIFICATION_HISTORY.md)
- Product gates and next scope: [PRODUCT_ROADMAP.md](PRODUCT_ROADMAP.md)
- Owner files for patches: [COMPONENT_INDEX.md](../../protocol/COMPONENT_INDEX.md)

Important rule for the next slow-path step:

```text
Do not start with the AI detector.
The fast path must keep working when any slow detector stalls or falls back.
```

Do not turn ROI/QP into the status headline. It is useful engineering and still
has benchmark coverage, but the product narrative is sensitive-region detection
feeding deterministic pre-encode masking.

## How to Investigate This Codebase

Useful questions:

- Explain the AetherFlow runtime pipeline step by step.
- What is the difference between `FrameScene`, `FrameRegion`, and `FrameDecision`?
- Why should Live Share Guard apply masks before encoder submission?
- Why is scene detection the backbone but not the v0.1 product by itself?
- Which files would be touched for a manual privacy mask fast path?
- What is the difference between NVENC and oneVPL in this project?
- Why should ROI/QP be optional instead of the main product?

Questions documentation alone cannot answer:

- Is this C++ change correct?
- Why did the build fail?
- Did this patch preserve COM / D3D11 lifetime rules?
- Is this trace artifact valid?
- Did this encoder backend regress latency?

For those, inspect source directly and use the repository verification workflow.

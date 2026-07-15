# AetherFlow Component Index

Use this file as the first stop for development agents. It points to likely files without requiring a full repository scan.

## Runtime Pipeline

- Entry point and pipeline scheduling: `src/main.cpp`
- Screen capture: `src/ScreenCapture.cpp`, `include/AetherFlow/ScreenCapture.h` (Windows capture-timing root-fix PD1 measure-first step, 2026-05-16: WGC `Direct3D11CaptureFrame.SystemRelativeTime` is now read while the frame is alive and exposed via the `LastFrameSystemRelativeTime()` / `LastFrameTimestampFromWgc()` accessors — lowest-blast-radius, no `CaptureTexture()` signature change; QPC fallback for the DXGI-duplication path / zero stamp. Propagated into `PipelineFrame` and surfaced as the always-on additive `captureDeltaMs` trace field + the run-end `[CAPTURE] effective capture fps …` stdout diagnostic. PD2a/PD3 **IMPLEMENTED 2026-05-17**: `--record-timed` / `AETHERFLOW_TIMED_RECORDING` now propagates the real stamp via `EncodeFrameRequest.captureTimestamp100ns` to the NVENC dedicated writer thread, which emits a 1:1 PTS sidecar `output_encoded.timestamps.txt` (mkvmerge timecodes v2); `run_scene_test.sh` muxes honoring it (mkvmerge per-frame, else ffmpeg real-average-fps). Flag unset = byte-identical canonical path, no sidecar; measurement diagnostic remains unconditional)
- Runtime deterministic scene/ROI/mask decisions, including panic mask producers: `include/AetherFlow/IAIFrameAnalyzer.h`
- Shared legacy runtime constants: `include/AetherFlow/Common.h`
- Legacy mouse-centered QP-map generator (Windows-only, retained for ROI experiments): `include/AetherFlow/ROIGenerator.h`, `src/ROIGenerator.cpp`
- UIAutomation password-field privacy mask producer (2026-05-17: the UIA subtree scan runs on a **dedicated background poll thread**; `Evaluate()` only copies a mutex-protected cached snapshot — never blocks the producer/capture thread): `include/AetherFlow/PasswordFieldPrivacyMaskModule.h`, `src/PasswordFieldPrivacyMaskModule.cpp`
- Deterministic visible notification/popup window privacy mask producer (2026-05-17: the `EnumWindows` + per-process scan runs on a **dedicated background poll thread**; `Evaluate()` only copies a cached snapshot): `include/AetherFlow/NotificationProducerModule.h`, `src/NotificationProducerModule.cpp`
- Live Share Guard privacy mask compositor, including blackout, blur, mosaic, and grayscale modes (grayscale added 2026-05-23 for the scene-demo `slides` class): `src/PrivacyMaskCompositor.cpp`, `include/AetherFlow/PrivacyMaskCompositor.h`
- Runtime mask wiring, CLI/env toggles, notification process whitelist / packaged-app aliases, mask mode selection, and pipeline trace fields: `src/main.cpp` (also hosts `RightCtrlPanicMaskHotkey`, a dev/test affordance for interactive panic-mask triggering — not a product feature)
- Async analyzer bridge (P2 + Bridge Hardening 2026-05-12; cross-platform pure C++ `IFrameDecisionModule` that wires any `IAIFrameAnalyzer` into the deterministic policy engine via non-blocking `SubmitFrame` + `TryGetLatest`; supports `submitEveryNFrames` sub-sampling, `dropWhenBusy` cadence, per-frame staleness `= currentFrameIndex - submitFrameIndex`, and `LastSubmitted()` / `LastContributed()` / `LastInferenceMs()` / `LastStalenessFrames()` trace accessors): `include/AetherFlow/AsyncAnalyzerBridgeModule.h`, `src/AsyncAnalyzerBridgeModule.cpp`
- Real-thread mock slow analyzer (first `IAIFrameAnalyzer` impl, used to verify bridge + confidence-based merge without pulling in a model runtime; `std::thread` worker + `std::mutex` + `std::condition_variable` + `std::atomic` shutdown, configurable inference delay via ctor / `AETHERFLOW_MOCK_ANALYZER_INFERENCE_MS` default 200 ms, drop-when-busy queue depth 1, lazy worker start on first `SubmitFrame`): `include/AetherFlow/MockSlowAnalyzer.h`, `src/MockSlowAnalyzer.cpp`
- Phase 4 P0.1 Windows ONNX scene classifier (landed and verified on **real captured pixels** 2026-05-15; CLIP zero-shot 5-class on ONNX Runtime + DirectML EP with CPU fallback; real-thread worker mirroring `MockSlowAnalyzer`; `SubmitFrame` does a producer-thread D3D11 staging-texture readback of the captured BGRA frame — `CopyResource`→`Map`→row-pitch-safe `memcpy`→`Unmap`, enqueue, return — i.e. **real screen pixels, not synthetic frame-index input**; worker does BGRA→RGB→224×224→CLIP normalize→ONNX→softmax→argmax; plugs into `AsyncAnalyzerBridgeModule` as a new `IAIFrameAnalyzer`; `#ifdef AETHERFLOW_ENABLE_SCENE_CLASSIFIER`-guarded; opt-in via `--scene-classifier-onnx-model=` / `--scene-classifier-provider=` / `AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL` / `AETHERFLOW_SCENE_CLASSIFIER_PROVIDER`; evidence `.aetherflow/runs/scene_classifier_onnx_smoke/verify_report.json` `scene_classifier.status: passed`, real-pixel proof `scene_class_distribution={mixed_ui:266, sensitive_surface:634}`): `include/AetherFlow/ai/SceneClassifierOnnx.h`, `src/ai/SceneClassifierOnnx.cpp`
- Phase 4 P0.1 cross-platform policy engine (landed 2026-05-15; pure C++ `IFrameDecisionModule` that maps the post-merge `FrameScene` into `PolicyDecision { mask_mode, encode_hint, mode_label, reason }` with a 150-frame / 5-second-at-30-fps switch floor plus 3-consecutive hysteresis, `confidence < 0.6` low-confidence fallback, and panic override that resets hysteresis; **advisory only for P0.1** — product consumers are trace + Studio status, while opt-in `SceneDemoActionModule` reads the stable class for a non-product compositor preview; encoder wiring is P1.1): `include/AetherFlow/policy/PolicyEngine.h`, `src/policy/PolicyEngine.cpp`, `include/AetherFlow/policy/PolicyDecision.h`
- Phase 4 §4.y opt-in visible scene demo action (landed 2026-05-16; header-only `IFrameDecisionModule` registered AFTER `PolicyEngine` so it observes the final merged `FrameScene`; emits ONE full-screen `PrivacyMask` `FrameRegion` `source="scene-demo-action"` whose visual mode is chosen per detected class (remapped 2026-05-23) — `sensitive_surface`→Blackout, `video`→Mosaic, `code_text`→Blur, `slides`→Grayscale, `mixed_ui`(generic desktop)/unknown→passthrough so a plain desktop is never obscured; `Grayscale` is a `PrivacyMaskMode` shader added to `PrivacyMaskCompositor`; suppressed when any deterministic producer mask already exists on the decision so real masks always win; does NOT call `ProposeScene`; **opt-in / default OFF, a crude visual proxy of P1.1, NOT product behavior**; OFF ⇒ module not registered ⇒ byte-equivalent; no trace/verify schema bump — reuses schema-v3 `privacyMaskSource`/`privacyMaskPath`; opt-in via `--scene-classifier-demo-action` / `AETHERFLOW_SCENE_CLASSIFIER_DEMO_ACTION`, only effective when the scene classifier is also active; mode selection plumbed in `src/main.cpp` Stage 3): `include/AetherFlow/SceneDemoActionModule.h`, `src/main.cpp` (flag parse + Stage 3 per-frame mode select)
- Vendored ONNX Runtime + DirectML (gitignored; recreate via SOP): `third_party/onnxruntime/SOURCE.md`

### Phase 4 Paths (P0.1 + cross-platform core LANDED 2026-05-15; later phases planned)

Phase 4 P0.1 (Windows ONNX scene classifier) and the cross-platform policy
core landed on 2026-05-15 — those subsections are marked **LANDED** and their
files exist on disk (also listed in the active Runtime Pipeline list above).
The macOS (P0.2) and Android (P1.0) subsections are still planned; file-based
agents should treat only the non-landed paths as reserved. Current product
sequencing is in [docs/3-product/PRODUCT_ROADMAP.md](../docs/3-product/PRODUCT_ROADMAP.md) §v0.2 /
Phase 4. Phase 4
priorities (revision v4, 2026-05-15) put **Windows (P0.1)** and **macOS
(P0.2)** in the must-ship tier, with **Android as P1.0** gated on suitable
hardware and an explicit Architecture scope decision.

#### Cross-platform core (shared across all platforms; P0.3) — LANDED 2026-05-15

The policy layer landed with Phase 4 P0.1 (it is the active runtime list above;
listed here for the phase map). The abstract `SceneClassifier` interface was
*not* needed — `IAIFrameAnalyzer` is the seam, so `SceneClassifierOnnx`
implements it directly.

- ~~Scene classifier abstract interface: `include/AetherFlow/ai/SceneClassifier.h`, `src/ai/SceneClassifier.cpp`~~ — not created; `IAIFrameAnalyzer` is the seam
- Policy engine (classifier output + telemetry → `PolicyDecision { mask_mode, encode_hint, mode_label, reason }` with hysteresis + low-confidence fallback): `include/AetherFlow/policy/PolicyEngine.h`, `src/policy/PolicyEngine.cpp` — **on disk**
- Policy decision struct: `include/AetherFlow/policy/PolicyDecision.h` — **on disk**

#### Phase 4 P0.1 — Windows (ONNX Runtime + DirectML) — LANDED 2026-05-15

- Windows ONNX RT + DirectML scene classifier adapter: `include/AetherFlow/ai/SceneClassifierOnnx.h`, `src/ai/SceneClassifierOnnx.cpp` — **on disk**
- Model checkpoint: `models/scene_classifier_v1.onnx` (gitignored; 335 MiB)
- Vendored ONNX Runtime: `third_party/onnxruntime/` (gitignored; SOP in `third_party/onnxruntime/SOURCE.md`; pinned `Microsoft.ML.OnnxRuntime.DirectML` 1.20.1)
- CLI / env opt-in: `--scene-classifier-onnx-model=models/scene_classifier_v1.onnx`, `--scene-classifier-provider=<directml|cpu>`, `AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL`, `AETHERFLOW_SCENE_CLASSIFIER_PROVIDER`
- Plugs into the already-landed `AsyncAnalyzerBridgeModule` (Bridge Hardening 2026-05-12) as a new `IAIFrameAnalyzer` implementation

#### Phase 4 P0.2 — macOS (CoreML + Neural Engine)

- macOS CoreML scene classifier adapter (Neural Engine preferred; GPU / CPU fallback): `include/AetherFlow/ai/SceneClassifierCoreML.h`, `src/ai/SceneClassifierCoreML.mm`
- Model checkpoint: `models/scene_classifier_v1.mlmodel` (converted from the P0.1 trained head via `coremltools`)

#### Phase 4 P1.0 — Android (LiteRT / NNAPI / GPU delegate, gated)

Gated on acquiring an Android device suitable for NNAPI/NPU validation and approving the platform scope in writing. Document the decision at the P1.0 checkpoint before creating any file under this section.

- LiteRT scene classifier adapter: `src/ai/SceneClassifierLiteRT.cpp`
- Android screen capture (MediaProjection + ImageReader / HardwareBuffer): `src/platform/android/AndroidScreenCapture.cpp`
- Android encoder (MediaCodec H.264 wrapper): `src/platform/android/AndroidEncoder.cpp`
- Android run-loop + JNI bridge: `src/platform/android/AndroidPlatformShim.cpp`
- Android Gradle project + AndroidManifest + JNI loader: `android/app/build.gradle`, `android/app/src/main/AndroidManifest.xml`, `android/app/src/main/java/...`
- Model checkpoint: `models/scene_classifier_v1.tflite`

#### Model artifacts (one trained head, multiple deployment formats)

- ONNX (Windows P0.1): `models/scene_classifier_v1.onnx`
- CoreML (macOS P0.2): `models/scene_classifier_v1.mlmodel`
- LiteRT (Android P1.0, gated): `models/scene_classifier_v1.tflite`
- Label manifest: `models/labels.jsonl`
- Encoder interface: `include/AetherFlow/IH264Encoder.h`
- macOS ScreenCaptureKit capture: `src/platform/mac/MacosScreenCapture.mm`, `include/AetherFlow/platform/mac/MacosScreenCapture.h`
- macOS run loop and run-artifact emission shim: `src/platform/mac/MacosPlatformShim.mm`, `include/AetherFlow/platform/mac/MacosPlatformShim.h`
- macOS secure-text-field privacy mask producer (phase-2 stub; phase-2 contract: the AXUIElement focus walk MUST run on a **dedicated background poll thread** — macOS analogue of the Windows UIA off-thread model — `Evaluate()` only copies a snapshot): `src/platform/mac/MacosSecureTextFieldPrivacyMaskModule.mm`, `include/AetherFlow/platform/mac/MacosSecureTextFieldPrivacyMaskModule.h`
- macOS deterministic notification / chat-window privacy mask producer (CGWindowListCopyWindowInfo + owner-name whitelist + z-order rect subtraction) (2026-05-18: the CGWindowList scan runs on a **dedicated background poll thread** with a time-based interval; `Evaluate()` only copies a mutex-protected cached snapshot — never blocks the producer/capture thread; mirrors the Windows `NotificationProducerModule` off-thread fix): `src/platform/mac/MacosNotificationProducerModule.mm`, `include/AetherFlow/platform/mac/MacosNotificationProducerModule.h`
- macOS privacy mask compositor (CoreImage + Metal, IOSurface BGRA pool; blackout/blur/mosaic + fail-closed clear-fill fallback): `src/platform/mac/MacosPrivacyMaskCompositor.mm`, `include/AetherFlow/platform/mac/MacosPrivacyMaskCompositor.h`
- macOS encoder boundary: `include/AetherFlow/platform/mac/IPlatformEncoderMac.h`

## Encoder Backends

- Intel oneVPL backend: `src/VplH264Wrapper.cpp`, `include/AetherFlow/VplH264Wrapper.h`
- NVIDIA NVENC backend: `src/NvencH264Wrapper.cpp`, `include/AetherFlow/NvencH264Wrapper.h`
- ROI defaults: `include/AetherFlow/VplDefaults.h`, `include/AetherFlow/NvencRoiDefaults.h`
- macOS VideoToolbox H.264 + AVAssetWriter MP4 mux: `src/platform/mac/VideoToolboxH264Encoder.mm`, `include/AetherFlow/platform/mac/IPlatformEncoderMac.h`

## Streaming Output (SRT live, 2026-07-03)

- Encoded-access-unit sink seam (drain-thread side, non-blocking contract; default no-op setter on `IH264Encoder`): `include/AetherFlow/IEncodedFrameSink.h`, `include/AetherFlow/IH264Encoder.h`
- SRT/MPEG-TS live output stage (bounded drop-oldest queue fed from NVENC `DrainSlot` / VPL `WriteBitstreamSample`; dedicated worker owns all libavformat state: `srt://0.0.0.0:<port>` listener, wait-for-keyframe + SPS/PPS prepend on mid-stream join, viewer-disconnect re-listen loop, `[SRT]` console stats; default OFF via `--srt-output` / `--srt-port=` / `AETHERFLOW_SRT_*`; compiled only when `third_party/ffmpeg` SDK present ⇒ `AETHERFLOW_ENABLE_SRT_OUTPUT`): `include/AetherFlow/streaming/SrtStreamOutput.h`, `src/streaming/SrtStreamOutput.cpp`
- AnnexB NAL helpers (pure logic, no FFmpeg dependency; SPS/PPS detect/extract for mid-stream join; unit-tested by `tests/test_annexb.cpp` ctest `AnnexBScan`): `include/AetherFlow/streaming/AnnexB.h`, `src/streaming/AnnexB.cpp`
- Vendored FFmpeg LGPL shared SDK (gitignored; pinned BtbN autobuild + SHA256; recreate via SOP): `third_party/ffmpeg/SOURCE.md`, `tools/fetch_ffmpeg.py`

## Studio UI + Packaging (spec Delta B/C, 2026-07-03)

- Shared pipeline entry (main()'s former Windows body as a parameterized function; CLI parse → `PipelineOptions` → `RunPipelineOnce`; Studio drives the SAME function on a worker thread — `AETHERFLOW_STUDIO_BUILD` guards out `main()` so the two front-ends cannot fork; `PipelineStatus` atomics feed the UI readouts, incl. AI scene readouts — classifier state/provider, canonical class index via the shared `kSceneClassNames` table, confidence, merge-source kind): `include/AetherFlow/app/PipelineRunner.h`, `src/main.cpp`
- Graceful stop (Ctrl+C / Ctrl+Break / console close latch → clean flush/report/`[SRT] summary` closeout; second signal force-quits; `AETHERFLOW_MAX_FRAMES=0` = run until stopped, stats retention capped at 100k frames with uncapped `deliveredFrames` denominators): `src/main.cpp` (`ConsoleStopHandler`, `g_stopRequested`)
- Settings window (Dear ImGui Win32+D3D11 shell; monitor/encoder/resolution/fps/bitrate/mask/AI-scene-toggle/SRT controls, Start/Stop, Panic latch, live status incl. per-class colored-dot AI scene indicator, viewer-URL copy; AI toggle auto-detects `scene_classifier_v1.onnx` at launch — env override → `<exe>/models/` → dev-tree `models/` — and renders grayed out when absent; `--ui-smoke` = render-only gate, `--ui-selftest` = programmatic Start→60-frame encode→clean-stop gate + a 150-frame AI leg when a model is on disk, asserting the classifier initializes and wins the scene merge ≥1 frame): `src/ui/StudioMain.cpp` → `AetherFlowStudio.exe`
- Persisted settings (hand-rolled INI `aetherflow_studio.ini` beside the exe; `Normalize()` clamps every field; unit-tested by ctest `AppConfigIni`): `src/ui/AppConfig.h`, `src/ui/AppConfig.cpp`, `tests/test_appconfig.cpp`
- Multi-monitor capture source (deterministic primary-first ordering; WGC `CreateForMonitor` + DXGI-duplication output matched by HMONITOR with loud fallback warning): `ScreenCapture::EnumerateMonitors` / `Init(..., monitorIndex)` in `include/AetherFlow/ScreenCapture.h`, `src/ScreenCapture.cpp`
- Runtime bitrate override (set before `Initialize`; NVENC + oneVPL honor it, compile-time `AETHERFLOW_BITRATE` stays the default): `IH264Encoder::SetTargetBitrateKbps`
- Vendored Dear ImGui (gitignored; pinned release + SHA256): `third_party/imgui/SOURCE.md`, `tools/fetch_imgui.py`
- Portable package builder (stages exes + runtime DLLs + app-local VC++ CRT + license notices + README; staged-folder self-test incl. a real SRT client probe before zipping): `tools/package_portable.py` → `output/AetherFlow-portable-<date>.zip`

## Configuration And Tools

- Compile-time runtime config: `include/AetherFlow/Config.h`
- ROI benchmark: `tools/roi_benchmark.ps1`
- Phase 4 P0.1 Stage A model export tool (offline, deterministic — seed=0, pinned `open_clip==3.3.0` / `torch==2.12.0`; CLIP ViT-B/32 visual encoder + frozen 5-prompt text embeddings; produces the gitignored `models/scene_classifier_v1.onnx`, ~335 MiB; runs offline, no runtime/agent dependency): `tools/export_clip_zeroshot.py`
- Agent run harness: `tools/agent_run.py` (`tools/agent_run.ps1` wrapper) (2026-05-18: the macOS path also publishes a byte-identical copy of the run mp4 to `<repo>/output/demo.mp4` every run via `publish_stable_output()` — stable eyeball path, copy not move, recorded as `stable_output` in `run_manifest.json`; Windows path unchanged — raw `output_encoded.h264`, `output/demo.mp4` still comes from `demo.sh` mux)
- Agent summarizer: `tools/agent_summarize.py` (`tools/agent_summarize.ps1` wrapper)
- Agent verifier: `tools/agent_verify.py` (`tools/agent_verify.ps1` wrapper) (2026-05-21: B1 surfaces `Capture Failures: X / Y` from `console.log` as `trace.capture_failures` + top-level `warnings` when ratio > 1.0 — warn-only, never flips status; B2 ships `proves` / `does_not_prove` on the `scene_classifier` block so `status: passed` no longer reads as accuracy-proven; each verify run also appends one JSONL row to `<repo>/.aetherflow/audit/ledger.jsonl` for longitudinal "is this governance layer actually catching things?" evidence)
- Agent effectiveness reporter: `tools/agent_report.py` (reads a run dir's `plan.md` / `verify_report.json` / `code_review.md` / `diagnosis.json`; classifies per-agent value events with honest severity — routine all-green runs are excluded, never logged; `--append` writes a curated entry to `docs/2-agent-system/AGENT_EFFECTIVENESS_LOG.md`)
- Longitudinal verifier ledger (one JSONL row per `agent_verify.py` run; quantitative; schema + invariants in `append_audit_ledger`, `tools/agent_verify.py:35`): `.aetherflow/audit/ledger.jsonl`
- Verifier evidence log (human-readable per-catch explanation; written by `append_evidence_log` / `collect_verify_events` only when a run warns or fails, with a catch-all so a failed run never leaves it empty): `.aetherflow/audit/evidence_log.md`
- Agent system effectiveness log (curated, accumulating, human-readable record of where the multi-agent workflow caught, diagnosed, or measured something real; appended by `tools/agent_report.py --append`; routine runs excluded): `docs/2-agent-system/AGENT_EFFECTIVENESS_LOG.md`
- Local password-field mask fixtures: `fixtures/password_field_fixture.html`, `fixtures/password_field_fixture.ps1`
- Local notification/window mask fixture: `fixtures/notification_fixture.ps1`
- Demo runners (one-shot Live Share Guard launcher with v0.2 defaults): `demo.ps1`, `demo.sh`
- Full-feature test runner (bash; runs `AetherFlow.exe` with every shippable feature on — deterministic masks + ONNX scene classifier + visible demo action + blur mode — then runs `agent_verify` so the capture-failure warning / `ledger.jsonl` / `evidence_log.md` audit trail is produced in one command): `run_full_test.sh`
- Scene-classifier eyeball runner (bash; classifier on + plain-language AI report, no verify gate): `run_scene_test.sh`
- Documentation map: `docs/DOCUMENTATION_INDEX.md`
- Agent collaboration guide: `docs/2-agent-system/AGENT_ARCHITECTURE.md`
- macOS verification scaffold: `docs/3-product/MACOS_AGENT_VERIFICATION.md`
- Current project status: `docs/1-status/PROJECT_STATUS.md`
- Dated verification evidence: `docs/4-qa-debugging/VERIFICATION_HISTORY.md`
- Current QA coverage and open validation matrix: `docs/4-qa-debugging/TROUBLESHOOTING_QA.md`
- Resolved recorded-video investigation: `docs/4-qa-debugging/JUDDER_INVESTIGATION.md`
- Agent-system effectiveness log (accumulating, cited per-run evidence that the multi-agent workflow caught/diagnosed/measured something real; appended by `tools/agent_report.py`): `docs/2-agent-system/AGENT_EFFECTIVENESS_LOG.md`
- Agent protocol documents: `protocol/AGENT_OPERABLE_ARCHITECTURE.md`, `protocol/AGENT_PROTOCOL.md`, `protocol/AGENT_COMMANDS.md`, `protocol/AGENT_CHANGELOG.md`
- Repo-level agent adapters: `AGENTS.md`, `CLAUDE.md`, `.github/copilot-instructions.md`, `.github/agents/*.md`, `.claude/agents/*.md`, `.codex/agents/*.toml`
- Repository-local agent skills: `.claude/skills/*/SKILL.md`, `.agents/skills/*/SKILL.md`

## Default Exclusions

Development agents must not read these areas unless a task card explicitly allows them:

- `third_party/`
- `build*/`
- `output/`
- `.claude/worktrees/`
- `.aetherflow/runs/`

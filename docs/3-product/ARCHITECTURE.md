# AetherFlow Architecture

This is the canonical product architecture for AetherFlow. Current verification
and release boundaries live in
[PROJECT_STATUS.md](../1-status/PROJECT_STATUS.md). File ownership lives in
[COMPONENT_INDEX.md](../../protocol/COMPONENT_INDEX.md).

## Contents

1. Product and scope
2. Design constraints
3. Two-lane system architecture
4. Platform pipelines
5. Pipeline stages
6. Scene-first decision system
7. Encoder backends and streaming
8. Privacy-mask system
9. Configuration
10. Output and telemetry
11. Development-agent workflow
12. Implementation status
13. Known gaps and next steps
14. File map
15. One-page reference

## 1. Product and Scope

AetherFlow is a Windows-first, cross-platform-ready real-time screen-media
pipeline. It captures a display, keeps frames GPU-resident, makes a scene-first
decision, applies deterministic privacy masks before encode, and encodes through
NVIDIA NVENC or Intel oneVPL. The macOS path uses ScreenCaptureKit and
VideoToolbox.

The first product wedge is **Live Share Guard**: reduce accidental disclosure
of sensitive content during screen sharing without putting a network or model
dependency on the media fast path.

The current repository is a pre-release prototype, not a certified DLP system.
Its claims are deliberately bounded by the named artifacts in project status
and QA documentation.

## 2. Design Constraints

### Runtime constraints

- 1080p30 target unless an operator selects another supported mode.
- Frames should remain GPU-resident through capture, mask, color conversion,
  and hardware encode.
- The frame path must not wait for remote services or an LLM.
- Slow or probabilistic analysis must be sampled, asynchronous, droppable, and
  fallback-safe.
- Deterministic privacy producers outrank advisory analysis.
- A mask-processing failure must never silently emit the same frame unmasked.
- Every important decision must be reconstructable from trace and run artifacts.

### Supported and planned platforms

| Platform | Current implementation |
|---|---|
| Windows | WGC/DXGI capture, D3D11 processing, NVENC or oneVPL, deterministic masks, ONNX classifier, SRT, Studio UI |
| macOS | ScreenCaptureKit, CoreImage/Metal masking, VideoToolbox, AVAssetWriter, deterministic chat-window producer |
| Android | Architecture reservation only; no adapter on disk |

## 3. Two-Lane System Architecture

```text
Fast lane (per frame, deterministic)
capture -> frame decision -> privacy mask -> color convert -> encode -> trace

Slow lane (sampled, advisory)
sampled frame -> local analyzer -> cached scene proposal -> policy/telemetry
```

### 3.1 Why the fast lane is GPU-resident

At 1920x1080, one BGRA frame is about 8 MiB. At 30 fps, a full GPU-to-CPU
readback and upload pair can move roughly 500 MiB/s before accounting for stalls,
cache effects, or synchronization. The production path therefore passes native
GPU textures between capture, compositor, color conversion, and encoder.

The ONNX classifier is an explicit exception: its opt-in sampling path performs
a D3D11 staging-texture readback in `SubmitFrame`, then queues CPU pixels for an
off-thread worker. That cost is not part of the classifier-inactive default path
and remains an optimization target.

### 3.2 Mask terminology

AetherFlow contains two unrelated concepts that must not be confused:

1. **NVENC emphasis/QP map** — an encoder quality hint. It does not hide pixels.
2. **Privacy mask** — a pixel transformation applied before encode. It can
   blackout, blur, or mosaic a region and is the Live Share Guard mechanism.

ROI/QP is an optional encoder optimization. Privacy masking is the product
security path.

## 4. Platform Pipelines

### 4.1 Windows

```text
Windows Graphics Capture / DXGI duplication
    -> ID3D11Texture2D BGRA
    -> FramePolicyEngine
       - panic/manual/password/window producers
       - baseline/cursor/async analyzer/policy/demo modules
    -> PrivacyMaskCompositor (BGRA, pre-encode)
    -> D3D11VideoConverter (BGRA -> NV12)
    -> NVENC or Intel oneVPL
    -> optional H.264 access-unit sink -> SRT/MPEG-TS
    -> frame_trace.jsonl + run artifacts
```

The default Windows mask mode is blur. Shader or non-blackout processing
failures fall back to blackout. If the compositor cannot complete the
fail-closed path, the pipeline must fail rather than silently encode the
original pixels.

### 4.2 macOS

```text
ScreenCaptureKit
    -> IOSurface-backed BGRA texture
    -> shared FramePolicyEngine concepts
    -> MacosPrivacyMaskCompositor (CoreImage + Metal)
    -> VideoToolbox H.264
    -> AVAssetWriter MP4
    -> trace + run artifacts
```

The macOS compositor supports blackout, blur, and mosaic and uses a clear-fill
fail-closed fallback. Current macOS failure semantics skip a frame after an
ultimate mask failure, while Windows aborts the run; semantic parity remains an
open architecture item.

### 4.3 Platform isolation

`CMakeLists.txt` separates Windows and Apple translation units. Shared headers
use `_WIN32` guards and forward declarations so cross-platform policy code does
not require the D3D11 SDK. The long-term boundary should replace leaked D3D11
types with explicit `NativeTexture` and `NativeDevice` aliases.

## 5. Pipeline Stages

### 5.1 Capture

`ScreenCapture` prefers Windows Graphics Capture and retains the DXGI
duplication fallback. WGC `SystemRelativeTime` is read while the frame is alive
and propagated into `PipelineFrame`; DXGI uses a QPC fallback when no WGC stamp
exists. Capture retries are surfaced in console and verification artifacts.

Multi-monitor selection uses deterministic primary-first enumeration and WGC
`CreateForMonitor`, with a loud fallback warning when the requested monitor
cannot be matched.

### 5.2 Frame decision

`FramePolicyEngine` executes deterministic `IFrameDecisionModule` instances in
registration order. Modules may propose a scene, emit quality regions, emit
privacy masks, or attach advisory policy. Confidence-based scene merge prevents
the baseline from blocking a stronger producer.

### 5.3 Privacy-mask composition

Mask rectangles are clipped to the frame and applied on BGRA pixels before
color conversion. Trace records the selected path, mask count, applied count,
fallback use, source, panic state, and CPU-side `maskMs` dispatch timing.
`maskMs` is not a GPU-completion measurement; a future `gpuMaskMs` requires GPU
timestamps.

### 5.4 Color conversion

`D3D11VideoConverter` converts BGRA to encoder-owned NV12 textures through
`VideoProcessorBlt`. The old compute-shader conversion path is retired.

### 5.5 Encode and drain

The encoder accepts an `EncodeFrameRequest`, writes into a bounded pool, and
drains bitstream asynchronously. Disk writes are owned by a dedicated writer
thread and must not hold the pipeline state mutex.

## 6. Scene-First Decision System

The policy model begins with what the frame contains, then chooses actions such
as masking, trace annotations, or future encoder hints. Cursor location alone
is not a sufficient content model: a cursor can sit over an editor while a
video region is the viewer's primary visual content.

### 6.1 Core data types

| Type | Responsibility |
|---|---|
| `FrameContext` | Frame index, dimensions, timing, native texture/device context |
| `FrameScene` | Scene type, confidence, source, and related metadata |
| `FrameRegion` | Rectangle, purpose, source, confidence, and optional visual mode |
| `FrameDecision` | Merged scene, quality regions, privacy masks, and policy fields |
| `PolicyDecision` | Advisory mask mode, encode hint, label, and reason |
| `FrameSceneType` | Closed scene taxonomy used by the current trace/schema |
| `FrameRegionPurpose` | Distinguishes quality regions from privacy masks |

`FrameDecision::ProposeScene()` performs a strict `>` confidence comparison.
Equal confidence preserves first-writer order, so deterministic module ordering
remains stable.

### 6.2 Module interfaces

`IFrameDecisionModule` is synchronous and runs on the producer thread:

```cpp
class IFrameDecisionModule {
public:
    virtual bool Initialize(ID3D11Device*, int width, int height) = 0;
    virtual void Evaluate(const FrameContext&, FrameDecision*) = 0;
    virtual const char* Name() const = 0;
    virtual void Warmup(const FrameContext&) {}
};
```

`IAIFrameAnalyzer` is the slow-path seam. Its implementation owns threading,
drop policy, and result caching; `AsyncAnalyzerBridgeModule` adapts its latest
result into the deterministic decision engine without waiting.

Concrete decision modules include:

- `PanicPrivacyMaskModule`
- `ManualPrivacyMaskModule`
- `PasswordFieldPrivacyMaskModule`
- `NotificationProducerModule`
- `BaselineSceneModule`
- `CursorFocusModule`
- `AsyncAnalyzerBridgeModule`
- `PolicyEngine`
- `SceneDemoActionModule`

### 6.3 Registration order

`src/main.cpp` contains nine `framePolicy.AddModule(...)` registrations. Each is
conditional except the baseline, and the relative order is intentional:

1. `PanicPrivacyMaskModule`
2. `ManualPrivacyMaskModule`
3. `PasswordFieldPrivacyMaskModule`
4. `NotificationProducerModule`
5. `BaselineSceneModule`
6. `CursorFocusModule`
7. `AsyncAnalyzerBridgeModule`
8. `PolicyEngine`
9. `SceneDemoActionModule`

The policy engine runs after scene producers so it sees the merged scene. The
demo action runs last and is suppressed when a deterministic producer already
emitted a privacy mask.

### 6.4 Deterministic producers

- **Panic:** full-frame mask from CLI/env or Studio panic latch.
- **Manual:** operator-supplied rectangles.
- **Password field:** foreground UI Automation `IsPassword=true` Edit controls.
  The expensive tree walk runs on a background poll thread; `Evaluate()` reads
  a cached snapshot.
- **Notification/window:** visible whitelisted top-level or popup windows with
  higher-z-order clipping and packaged-application alias handling. Enumeration
  and process queries run on a background poll thread.
- **Baseline:** stable fallback scene with confidence 0.5.
- **Cursor focus:** optional quality region, disabled by default.

### 6.5 Local analyzer and policy

`SceneClassifierOnnx` implements `IAIFrameAnalyzer` with ONNX Runtime,
DirectML preferred and CPU fallback. It classifies five labels:

- `code_text`
- `slides`
- `video`
- `mixed_ui`
- `sensitive_surface`

`PolicyEngine` applies a 150-frame (5-second at 30 fps) switch floor,
three-consecutive confirmation, a
confidence-under-0.6 fallback, and panic override reset. In the current product
it is advisory: it writes trace and Studio status but does not drive NVENC,
oneVPL, or VideoToolbox parameters. The opt-in `SceneDemoActionModule` also
reads the stable class to drive a non-product compositor preview.

The opt-in `SceneDemoActionModule` is a visual prototype, not product behavior:

| Class | Demo outcome |
|---|---|
| `sensitive_surface` | blackout |
| `video` | mosaic |
| `code_text` | blur |
| `slides` | grayscale |
| `mixed_ui` / unknown | passthrough |

Grayscale is internal to this demo mapping and is not accepted by the general
`--privacy-mask-mode` or `AETHERFLOW_PRIVACY_MASK_MODE` option.

## 7. Encoder Backends and Streaming

All Windows backends implement
[IH264Encoder](../../include/AetherFlow/IH264Encoder.h).

### 7.1 NVIDIA NVENC

Implementation:
[NvencH264Wrapper.cpp](../../src/NvencH264Wrapper.cpp).

Key properties:

- encoder-owned registered D3D11 NV12 texture pool;
- asynchronous submit and drain;
- dedicated bitstream writer thread;
- optional ROI/emphasis map support;
- encoded-access-unit sink callback for SRT;
- runtime bitrate override before initialization.

The NVENC header is not redistributed and must be fetched separately. When the
header or supported runtime is unavailable, CMake/runtime selects another
available backend.

### 7.2 Intel oneVPL

Implementation:
[VplH264Wrapper.cpp](../../src/VplH264Wrapper.cpp).

oneVPL is vendored for build availability. Actual encoding still requires a
supported Intel GPU and driver. The wrapper uses video-memory surfaces, writes
encoded samples, and exposes the same access-unit sink seam. Current release
status does not claim Intel hardware parity without a named machine artifact.

### 7.3 macOS VideoToolbox

`VideoToolboxH264Encoder.mm` implements the platform-specific encoder boundary
and feeds AVAssetWriter for MP4 output. The current macOS wrapper bridges the
callback model through `IPlatformEncoderMac`; a truly cross-platform async
encoder interface remains future work.

### 7.4 ROI/QP boundary

Structured quality regions reach the encoder boundary, but current backends use
only `qualityRegions.front()`. Multi-region ordering, clamping, backend limits,
and applied-region reporting remain deferred encoder work.

### 7.5 SRT output

`SrtStreamOutput` is an optional post-encoder stage:

```text
H.264 access units
  -> bounded drop-oldest queue
  -> dedicated libavformat worker
  -> MPEG-TS over srt://0.0.0.0:<port>
```

The worker waits for a keyframe on join, prepends cached SPS/PPS when needed,
and returns to listen after viewer disconnect. It is single-viewer, video-only,
and disabled unless the vendored FFmpeg SDK is present and the operator opts in.

## 8. Privacy-Mask System

### 8.1 Visual modes

| Mode | Public selection | Notes |
|---|---|---|
| Blackout | CLI/env/Studio | Fail-closed fallback |
| Blur | CLI/env/Studio | Windows default |
| Mosaic | CLI/env/Studio | macOS default and selectable on Windows |
| Grayscale | Internal scene demo only | Not a general product-mode value |

Windows trace paths include:

- `d3d11-bgra-clearview`
- `d3d11-bgra-blur-shader`
- `d3d11-bgra-mosaic-shader`
- `d3d11-bgra-grayscale-shader`
- `d3d11-bgra-clearview-fallback`

macOS uses corresponding `coreimage-bgra-*` paths.

### 8.2 Producer priority

Deterministic privacy masks are accumulated before the baseline/advisory scene
logic. A deterministic producer mask suppresses the scene demo action. Panic
always provides a full-frame protection path.

### 8.3 Claim boundary

Fixture evidence proves the fixture condition. Teams alias evidence proves the
observed executable/config/window identity. Neither supports a claim that every
application, notification form, password control, or sensitive surface is
covered. The complete matrix is maintained in QA documentation.

## 9. Configuration

### 9.1 Compile-time defaults

`include/AetherFlow/Config.h` contains legacy/default values such as frame
dimensions, frame rate, bitrate, GOP, queue depth, and ROI constants. Backend
defaults are normalized through `NvencRoiDefaults.h` and `VplDefaults.h`.

### 9.2 Runtime controls

Runtime configuration is owned primarily by `src/main.cpp` and Studio. Major
surfaces include:

- encoder, monitor, resolution, fps, and bitrate;
- password-field and notification-mask enablement;
- notification process whitelist and packaged-app aliases;
- privacy mask mode and panic mask;
- manual rectangles and cursor ROI;
- ONNX model/provider and scene demo action;
- SRT port, latency, and passphrase;
- output directory, maximum frames, and timed recording.

Exact flags and environment names are maintained in
[OPERATION_GUIDE.md](../OPERATION_GUIDE.md) and must be verified against source
before release documentation changes.

## 10. Output and Telemetry

### 10.1 Outputs

- `output_encoded.h264` — Windows encoded elementary stream when enabled.
- `output_encoded.timestamps.txt` — optional mkvmerge timecodes-v2 sidecar for
  timed recording.
- `frame_trace.jsonl` — per-frame trace.
- `output/demo.mp4` — stable demo path produced by platform/demo tooling, not by
  every canonical Windows run.
- `.aetherflow/runs/<run_id>/` — local run artifacts.

### 10.2 Trace schema

The trace records frame/scene sources, confidence, decision sources, quality and
privacy counts, selected mask path, fallback use, panic state, per-stage timing,
capture timing, encode result, analyzer bridge state, classifier metrics, and
advisory policy fields.

`agent_summarize.py` reduces the trace. `agent_verify.py` applies evidence gates
and writes `verify_report.json`. A classifier-inactive run reports the optional
classifier block as `not_applicable` instead of implying accuracy proof.

## 11. Development-Agent Workflow

AetherFlow's development agents follow the same repository protocol across
GitHub, Claude, and Codex:

```text
Observe -> Architecture Planning Gate -> approval -> plan.md
        -> reproduce -> classify -> patch -> build -> smoke
        -> benchmark when required -> verify -> independent review
        -> one scoped repair when required -> docs sync -> report
```

Canonical references:

- [AGENT_ARCHITECTURE.md](../2-agent-system/AGENT_ARCHITECTURE.md)
- [AGENT_COMMANDS.md](../../protocol/AGENT_COMMANDS.md)
- [AGENT_PROTOCOL.md](../../protocol/AGENT_PROTOCOL.md)
- [COMPONENT_INDEX.md](../../protocol/COMPONENT_INDEX.md)

The three adapter surfaces each contain seven matching role files. Repair maps
to the `debug-verifier` adapter. Architecture synchronization is a Claude/Codex
skill, not another role. Native-host delegation and scheduled external loops
remain explicitly unverified.

## 12. Implementation Status

This table describes implementation, not complete release verification. Use
[PROJECT_STATUS.md](../1-status/PROJECT_STATUS.md) for evidence boundaries.

| Component | State | Notes |
|---|---|---|
| Windows WGC/DXGI capture | Implemented | Retry behavior and timestamp diagnostics exist; current retry warning remains open. |
| BGRA to NV12 | Implemented | D3D11 `VideoProcessorBlt` path |
| Producer-consumer pipeline | Implemented | Bounded queues and async encoder drain |
| NVENC backend | Implemented and locally verified | NVIDIA hardware path |
| oneVPL backend | Implemented/build-covered | Current Intel hardware artifact missing |
| macOS ScreenCaptureKit/VideoToolbox | Implemented/historically verified | Not rerun on current Windows host |
| Scene-first decision model | Implemented | Confidence-based merge |
| Deterministic mask producers | Implemented | Panic, manual, password, notification/window |
| Windows pre-encode compositor | Implemented | Blackout, blur, mosaic, internal demo grayscale |
| macOS compositor/window producer | Implemented | CoreImage/Metal and CGWindowList path |
| macOS secure-text producer | Stub | AXUIElement implementation missing |
| Async analyzer bridge | Implemented/verified | Sample, drop, staleness, cached result |
| Windows ONNX classifier | Implemented/partially verified | Plumbing proven; real-screen accuracy unmeasured |
| Policy engine | Implemented/advisory | Encoder steering not wired |
| Scene demo action | Implemented/opt-in | Not product behavior; five-class sweep incomplete |
| SRT output | Implemented/loopback verified | Single viewer; field conditions incomplete |
| Studio UI | Implemented/partially verified | Deterministic self-test plus open interaction paths |
| Trace/summarizer/verifier | Implemented | Capture retry remains warn-only |
| Agent development system | Implemented/static parity verified | Native-host/scheduler proof incomplete |
| Android runtime | Not implemented | Reserved paths only |

## 13. Known Gaps and Next Steps

### 13.1 Architecture gaps

| ID | Gap | Impact | Direction |
|---|---|---|---|
| M1 | Confidence values mix baseline constants, deterministic certainty, and model probability | Merge semantics are not calibrated | Define and document calibrated confidence semantics. |
| M2 | No post-merge `SceneStabilizerModule` | Borderline classifier output can flip the merged scene even though policy telemetry has hysteresis | Add a final merge-layer stabilizer if product behavior begins consuming the scene. |
| M3 | `FrameSceneType` is a closed enum | Top-k distribution information is lost | Revisit after the classifier accuracy checkpoint. |
| M4 | Encoders use only `qualityRegions.front()` | Multi-region ROI is discarded | Add normalized ordering, clamp rules, backend caps, and applied-region trace. |
| M5 | `IAIFrameAnalyzer.h` owns too many concrete types | Compile-time and ownership clarity | Split stable interfaces and concrete modules into focused headers. |
| X1 | Shared interfaces leak D3D11 types | Future platform adapter ABI friction | Introduce platform-native type aliases. |
| X2 | Encoder abstraction is fundamentally synchronous | VideoToolbox/MediaCodec require callback or queue adaptation | Add an async submission/result contract when Android work begins. |
| MAC1 | Secure-text producer is a stub | No macOS password-field masking | Implement AXUIElement scanning on a background poll thread. |
| MAC2 | macOS ROI/QP unsupported | Advisory hints do not influence VideoToolbox | Evaluate pixel-domain steering or platform-supported alternatives. |
| MASK1 | CPU `maskMs` is not GPU-completion time | Under-4-ms full-stage claim is unproven | Add D3D11/Metal GPU timestamps. |
| VERIFY1 | WGC retry ratio is warn-only | A passed smoke can coexist with poor capture delivery | Define a justified reliability gate from live-content evidence. |

Closed structural gaps:

- Confidence-based merge (`FrameDecision::ProposeScene`) is implemented.
- `AsyncAnalyzerBridgeModule` connects local analyzers to policy.
- Windows/macOS platform source isolation exists in CMake.
- Blur and mosaic compositor modes are implemented.
- macOS notification scanning moved off the real-time frame thread.

### 13.2 Product sequence

1. Record an uninterrupted real-time masking demo with bounded claims.
2. Close live application and visual-mode evidence gaps.
3. Decide source-only pre-release versus rebuilt/signed binary release.
4. Run Intel and macOS current-tree parity gates.
5. Measure scene-classifier quality before wiring policy into product behavior.

Detailed milestone sequencing lives in
[PRODUCT_ROADMAP.md](PRODUCT_ROADMAP.md). Detailed unverified paths live in
[TROUBLESHOOTING_QA.md](../4-qa-debugging/TROUBLESHOOTING_QA.md).

## 14. File Map

```text
<repo>/
|-- README.md                         public entry and quick start
|-- AGENTS.md / CLAUDE.md             agent entry contracts
|-- DESIGN_DECISIONS.md               non-obvious design rationale
|-- CMakeLists.txt                     platform/build composition
|-- include/AetherFlow/               shared and platform headers
|-- src/                              Windows/shared implementation
|   |-- ai/                           ONNX classifier
|   |-- policy/                       advisory policy engine
|   |-- streaming/                    SRT output and Annex-B helpers
|   |-- ui/                           Studio UI and persisted settings
|   `-- platform/mac/                 macOS capture/mask/encode shim
|-- tools/                            run, summarize, verify, package, fetch
|-- protocol/                         canonical agent protocol and component index
|-- docs/
|   |-- 1-status/PROJECT_STATUS.md    concise current release boundary
|   |-- 2-agent-system/               agent architecture and effectiveness
|   |-- 3-product/                    architecture, code map, roadmap
|   `-- 4-qa-debugging/               QA coverage, history, investigations
|-- .github/agents/                   GitHub role adapters
|-- .claude/agents/                   Claude role adapters
|-- .codex/agents/                    Codex role adapters
|-- .claude/skills/                   Claude repository-local skills
|-- .agents/skills/                   Codex repository-local skills
`-- .aetherflow/                      ignored/local run and audit artifacts
```

### 14.1 Core ownership quick map

| Path | Responsibility |
|---|---|
| `src/main.cpp` | Pipeline runner, CLI/env parsing, module wiring, trace, stop behavior |
| `include/AetherFlow/IAIFrameAnalyzer.h` | Decision data model and baseline/cursor modules |
| `include/AetherFlow/AsyncAnalyzerBridgeModule.h` / `src/AsyncAnalyzerBridgeModule.cpp` | Non-blocking analyzer bridge |
| `src/PasswordFieldPrivacyMaskModule.cpp` | Background UI Automation password producer |
| `src/NotificationProducerModule.cpp` | Background window/process producer |
| `src/PrivacyMaskCompositor.cpp` | Windows D3D11 pre-encode mask stage |
| `src/ai/SceneClassifierOnnx.cpp` | Windows real-pixel ONNX classifier |
| `src/policy/PolicyEngine.cpp` | Advisory policy and hysteresis |
| `src/NvencH264Wrapper.cpp` | NVIDIA encode, drain, writer, access-unit sink |
| `src/VplH264Wrapper.cpp` | Intel oneVPL encode and sink |
| `src/streaming/SrtStreamOutput.cpp` | SRT/MPEG-TS worker |
| `src/ui/StudioMain.cpp` | Windows settings/status UI |
| `src/platform/mac/` | macOS capture, mask, encode, and run loop |
| `tools/agent_run.py` | Build and smoke artifact creation |
| `tools/agent_summarize.py` | Trace reduction |
| `tools/agent_verify.py` | Artifact judgment and optional benchmark gate |

The complete maintained map is
[CODE_COMPONENT_SUMMARY.md](CODE_COMPONENT_SUMMARY.md); the canonical routing
index is [COMPONENT_INDEX.md](../../protocol/COMPONENT_INDEX.md).

## 15. One-Page Reference

```text
PRODUCT
  Live Share Guard: deterministic pre-encode privacy masking

FAST PATH
  capture -> decide -> mask -> convert -> hardware encode -> trace

SLOW PATH
  sampled pixels -> local analyzer -> cached advisory scene/policy

WINDOWS
  WGC/DXGI + D3D11 + NVENC/oneVPL + optional SRT + Studio

MACOS
  ScreenCaptureKit + CoreImage/Metal + VideoToolbox + AVAssetWriter

SAFETY
  deterministic producers outrank AI; mask failure never silently emits
  the original frame; no network/LLM dependency in the runtime

EVIDENCE
  run_manifest.json + frame_trace.jsonl + trace.summary.json
  + verify_report.json + independent code_review.md
```

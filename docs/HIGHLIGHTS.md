# AetherFlow — 60-Second Technical Skim

## Project

AetherFlow is an **author-directed, AI-assisted native real-time media systems
portfolio project**. Its Windows path uses Windows Graphics Capture or DXGI,
D3D11 GPU surfaces, scene-first frame decisions, pre-encode privacy masking,
BGRA-to-NV12 conversion, and a shared NVIDIA NVENC / Intel oneVPL H.264
boundary. Encoded access units can feed an optional video-only SRT output. A
macOS path uses ScreenCaptureKit, CoreImage/Metal, VideoToolbox, and
AVAssetWriter. The first product wedge is **Live Share Guard**.

It is a pre-release masking prototype, not a certified DLP product or turnkey
consumer release.

## Fast Path / Slow Path

```text
FAST — deterministic, per frame
capture -> decide -> privacy mask -> convert -> hardware encode -> SRT / trace

SLOW — sampled, droppable, advisory
sampled pixels -> local ONNX analyzer -> cached scene -> status / trace
```

The fast path never waits for a model, network service, or LLM. Local producers
find password fields, selected application windows, manual regions, and panic
state; masking happens before encode. Windows falls back to blackout when a
non-blackout effect fails and aborts if the fail-closed path cannot complete.
macOS instead skips a frame after an ultimate mask failure, so the semantics are
not yet identical.

The optional roughly 1 Hz ONNX path runs off-thread. It does not steer NVENC,
oneVPL, or VideoToolbox; its opt-in full-screen effect is a demo, not product
privacy behavior.

## Engineering Signal

- GPU-resident D3D11 mask, color-conversion, and encode stages.
- Shared `IH264Encoder` abstraction with encoder-owned surfaces, asynchronous
  drain, and an H.264 sink used by SRT.
- Deterministic safety decisions outrank fallible advisory analysis.
- Frame-level traces and verifier reports preserve timing, mask, capture, and
  encode evidence instead of relying on screenshots or build success alone.
- Native Windows and macOS media stacks behind platform-specific boundaries.

## Measurements

> Point-in-time results from named runs; re-run before quoting as current.

| Evidence | Result | Boundary |
|---|---|---|
| Windows smoke, `evidence_refresh_20260723` | **900/900 encoded**, 0 encode failures/parse errors | Static-desktop WGC retry ratio 8.979 is warn-only; a same-day moving-content run logged **0/300 retries**. |
| First-party tests | **CTest 4/4** | Selected behavior, not exhaustive coverage. |
| `line_window_mask_live_20260723` | **199/200 frames masked** | A real installed LINE window; other messengers remain fixture-level. |
| Historical deterministic decision timing | p99 **0.17 ms** | CPU timing; both deterministic producers enabled. |
| `scene_classifier_onnx_20260723` | p95 **13.235 ms**, 4/4 gates | Off-thread timing; accuracy unmeasured. |
| `srt_loopback_refresh_20260723` | **90 frames decoded** | Local NVENC/FFmpeg loopback; single viewer, no field-network or Intel proof. |
| `mac_chat_window_mosaic_masked` | mean **5.40 ms**, p99 **7.14 ms** | Historical CPU dispatch timing with 11 rectangles/frame, not GPU completion. |

## Honest Scope

- Windows/NVIDIA is the primary locally verified path. oneVPL is implemented
  and build-covered but lacks a current Intel-hardware artifact.
- Password and application-window evidence is fixture/application specific; it
  does not prove universal detection.
- macOS evidence is historical; secure-text detection is a stub and ROI/QP is
  unsupported.
- ONNX is optional and manually supplied; accuracy and encoder steering are
  unproven. SRT is opt-in, video-only, and single-viewer.
- There is no formal version, tag, signed package, or GitHub Release; a
  current unsigned portable zip (2026-07-23) passed its staged self-tests but
  is unpublished.

Current truth: [PROJECT_STATUS](1-status/PROJECT_STATUS.md) · full design:
[ARCHITECTURE](3-product/ARCHITECTURE.md) · dated evidence:
[VERIFICATION_HISTORY](4-qa-debugging/VERIFICATION_HISTORY.md).

## Development Provenance

This is **author-directed, AI-assisted engineering**. The author defined and
directed the problem, architecture, constraints, acceptance criteria,
verification gates, and review boundaries. AI coding agents produced much of
the implementation and documentation; the project does not imply manual
authorship of every line.

The seven-role workflow is the development method, not the primary achievement:
[AGENT_ARCHITECTURE](2-agent-system/AGENT_ARCHITECTURE.md). Setup and detailed
operations: [README](../README.md) · [OPERATION_GUIDE](OPERATION_GUIDE.md).

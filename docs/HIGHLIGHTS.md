# AetherFlow — Highlights

A Windows-first, **GPU-resident screen-capture → H.264 pipeline** with deterministic
pre-encode privacy masking, NVIDIA NVENC / Intel oneVPL backends, optional SRT output,
and an advisory on-device AI slow path.

> The 60-second technical skim. For setup see the [README](../README.md); for the
> full design see [ARCHITECTURE](3-product/ARCHITECTURE.md); for current claim
> boundaries see [PROJECT_STATUS](1-status/PROJECT_STATUS.md).

## In 30 seconds

AetherFlow captures the screen, keeps frames GPU-resident, applies deterministic
privacy decisions before encode, and encodes through NVENC or Intel oneVPL. AI is
allowed only on a sampled, droppable slow path. The organizing principle is:

> **A realtime media system where probabilistic AI can fail safely without blocking
> the deterministic fast path.**

## Two lanes

- **Fast lane** — deterministic and per-frame: capture → scene / mask decision →
  pre-encode privacy composition → NV12 conversion → hardware encode → trace.
- **Slow lane** — sampled and advisory: an on-device ONNX scene classifier runs on
  its own thread and publishes a cached proposal. It never gates product masking or
  encoding.

## What carries the weight

1. **Cross-platform native GPU media engineering** — Windows WGC / DXGI + D3D11 +
   NVENC / oneVPL; macOS ScreenCaptureKit + Metal/CoreImage + VideoToolbox.
2. **Deterministic, fail-closed privacy behavior** — password-field, recognized
   chat-window, manual, and panic masks are applied before encode.
3. **Measurable system boundaries** — frame-level traces, named verification runs,
   current capability matrices, and explicit separation between implemented,
   verified, partially verified, and unverified paths.
4. **Author-directed, AI-assisted development** — coding agents produced much of the
   implementation under architecture, verification, repair, and review gates defined
   by the author.

## Measurable results

> Point-in-time figures from recorded runs. Re-run before quoting them as current
> release measurements.

| Metric | Value | Source |
|---|---|---|
| Windows default smoke | **120/120 encoded**, 0 encode failures, 0 trace parse errors | [Project status](1-status/PROJECT_STATUS.md) |
| First-party tests | **CTest 4/4** | `public_release_hardening_20260714` |
| Deterministic decision path | **0.17 ms p99**, 0% ≥10 ms in the historical interactive run | [Verification history](4-qa-debugging/VERIFICATION_HISTORY.md) |
| ONNX scene inference | **15.254 ms p95** off-thread | `scene_classifier_onnx_smoke` |
| macOS mask stage | mean **5.40 ms** / p99 **7.14 ms** with 11 rectangles per frame | `mac_chat_window_mosaic_masked` |
| SRT live loopback | **90 frames decoded** by a local FFmpeg client | `srt_output_v1` |

Raw `.aetherflow/runs/` bundles are gitignored because traces and recordings may
contain captured-screen-sensitive data. Public summaries remain in
[PROJECT_STATUS](1-status/PROJECT_STATUS.md) and
[VERIFICATION_HISTORY](4-qa-debugging/VERIFICATION_HISTORY.md).

## Current boundary

This repository is a **pre-release source snapshot**, not a formal binary release and
not a certified DLP product. Windows is the primary verified path. Intel parity,
broader privacy coverage, long-duration capture reliability, current macOS reruns,
and release packaging/signing remain open or partially verified.

## Development provenance

AetherFlow does not present itself as line-by-line manual authorship. The author defined
the product problem, architecture, system constraints, acceptance criteria, and review
gates; AI coding agents produced much of the implementation and documentation under the
repository workflow.

Details:

- [Agent architecture](2-agent-system/AGENT_ARCHITECTURE.md)
- [Agent effectiveness log](2-agent-system/AGENT_EFFECTIVENESS_LOG.md)
- [Design decisions](../DESIGN_DECISIONS.md)

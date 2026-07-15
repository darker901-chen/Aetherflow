# AetherFlow — Highlights

A Windows-first, **GPU-resident fast-path** screen-capture → H.264 (NVIDIA NVENC / Intel oneVPL)
pipeline with a **deterministic, scene-first privacy layer** that masks sensitive
content (password fields, chat-app windows, notifications) **before** it is
encoded — fully on-device, no cloud, no LLM in the runtime. First product wedge:
**Live Share Guard**.

> The 60-second skim. For setup see [README](../README.md); for the full design
> see [ARCHITECTURE](3-product/ARCHITECTURE.md); for the dev-agent workflow see
> [AGENT_ARCHITECTURE](2-agent-system/AGENT_ARCHITECTURE.md).

## In 30 seconds

AetherFlow captures the screen, keeps every frame GPU-resident, makes a
**scene-first** decision per frame, applies deterministic privacy masks **before**
encode, and encodes through NVENC or Intel oneVPL. AI is allowed only on a
droppable ~1 Hz slow path; the media fast path stays deterministic, measurable,
and fallback-safe. The organizing principle: **a realtime system where AI can
fail safely.**

## Two lanes

- **Fast lane** — deterministic, non-blocking, ~33 ms/frame budget: capture →
  scene-first decision → pre-encode mask → NV12 convert → encode. Media surfaces
  stay GPU-resident through mask, conversion, and hardware encode; deterministic
  decision work does not block on the slow analyzer.
- **Slow lane** — probabilistic, droppable, ~1 Hz, advisory: an on-device ONNX
  scene classifier on its own thread, decoupled by the `AsyncAnalyzerBridge` seam.
  Default product behavior uses it for trace and Studio status; it never gates
  the encoder or product masks. An explicit opt-in, non-product demo can map the
  stable class to a full-screen compositor preview.

## What carries the weight

1. **A self-built 7-role agent development workflow** — file-based artifacts,
   verifier-gated evidence flow, autonomous detect → repair → re-verify, and a
   cited [effectiveness log](2-agent-system/AGENT_EFFECTIVENESS_LOG.md).
2. **Cross-platform native GPU media craft** — Windows WGC + D3D11 + NVENC /
   Intel oneVPL; macOS ScreenCaptureKit + VideoToolbox + AVAssetWriter.
3. **A deterministic scene-first runtime** — frame-level trace schema v3,
   independent diff review, and a longitudinal audit ledger.

## Measurable results

> ⚠️ Point-in-time figures from specific recorded runs (sources below). Re-run to
> confirm before quoting.

| Metric | Value | Source |
|---|---|---|
| Current Windows default smoke | **120/120 encoded**, 0 encode failures, 0 parse errors | [Project status](1-status/PROJECT_STATUS.md); WGC retry warning disclosed there |
| First-party unit tests | **CTest 4/4** | `public_release_hardening_20260714` |
| `decisionMs` p99 with both deterministic producers enabled | **0.17 ms**, 0% ≥10 ms (historical interactive run) | [Verification history](4-qa-debugging/VERIFICATION_HISTORY.md), 2026-05-17 |
| ONNX scene inference p95 (off-thread) | **15.254 ms** | `scene_classifier_onnx_smoke` |
| macOS mask-stage `mask_ms` | mean **5.40 ms** / p99 **7.14 ms** (11 rectangles/frame) | `mac_chat_window_mosaic_masked` |
| SRT live loopback | **90 frames decoded** by a local FFmpeg client | `srt_output_v1` |

Raw `.aetherflow/runs/` bundles are intentionally gitignored because traces and
recordings may contain captured-screen-sensitive data. Public summaries remain
in [PROJECT_STATUS.md](1-status/PROJECT_STATUS.md) and
[VERIFICATION_HISTORY.md](4-qa-debugging/VERIFICATION_HISTORY.md); reproduce the
figures locally before relying on them.

## Why it is designed this way

See [DESIGN_DECISIONS.md](../DESIGN_DECISIONS.md) — the non-obvious choices in the
author's own words (e.g. DD-1: a mask failure never encodes the frame unmasked;
macOS skips it, while Windows currently aborts the run pending alignment; DD-2:
the runtime stays deterministic, the classifier is advisory).

# AetherFlow QA Coverage and Troubleshooting

This document has two purposes:

1. define the current verification boundary and the next gate for every major
   product surface;
2. preserve root-caused graphics, encoder, and synchronization failures that
   are likely to recur.

A green result applies only to the named platform, backend, path, and condition.
It must not be generalized into an all-features claim. Current release status is
owned by [PROJECT_STATUS.md](../1-status/PROJECT_STATUS.md); dated detail is in
[VERIFICATION_HISTORY.md](VERIFICATION_HISTORY.md). Agent-discovered blockers,
risks, diagnoses, and measurements are indexed in
[AGENT_EFFECTIVENESS_LOG.md](../2-agent-system/AGENT_EFFECTIVENESS_LOG.md); the
reusable technical incidents are retained below.

## QA Coverage Registry (2026-07-14)

| Surface | Status | Existing evidence | Not yet verified / next gate |
|---|---|---|---|
| Windows local build and first-party tests | **Verified on the current runtime tree** | `AetherFlow`, `AetherFlowStudio`, and four test executables built; CTest 4/4; first-party MSVC targets use `/utf-8` | Confirm hosted GitHub Actions on the final pushed commit; optional SDK paths compile out in a clean checkout and therefore are not hosted hardware proof. |
| Windows default smoke and trace | **Passed with warning** | `public_release_hardening_20260714`: 120/120 encoded, 0 encode failures, 0 parse errors | Nearly static WGC recorded 1007 retries for 120 delivered frames (ratio 8.392). Run live moving content and define whether a retry threshold becomes a hard gate. |
| Password-field mask | **Fixture-verified / application coverage partial** | Fixture emitted and applied masks; UI Automation scanning now runs on a background poll thread | Coverage is foreground `IsPassword=true` Edit controls, not every browser or custom control. Record a broader real-application matrix. |
| Notification/chat-window mask | **Fixture + Teams alias verified** | Local Win32 fixture and `teams_notification_alias_fix` | Record canonical live LINE, Slack, Discord, Telegram, and WhatsApp runs. A baseline with no visible target is not evidence. |
| Windows visual mask modes | **Implemented / partial visual evidence** | Product modes are blackout, blur, and mosaic; blur is the Windows default and blackout fallback. Internal demo logic also uses grayscale for slides | Record an uninterrupted source-versus-output sweep of all public modes and the internal grayscale demo outcome. The scene demo historically exercised only two of five classes. |
| Panic mask | **Verified** | Startup panic path applied a full-frame mask on every frame in the canonical run | Record the final product UX trigger separately from the developer Right Ctrl affordance. |
| NVENC backend | **Locally verified on NVIDIA** | Current/default and historical runs encoded with zero failures | Run a long-duration capture/encode soak and investigate inflight-slot, lock-busy, age-spike, and historical off-by-one observations. |
| Intel oneVPL backend | **Implemented; current hardware gate missing** | Source and build coverage | Run on supported Intel hardware; verify frame type, timestamps, access-unit sink, SRT, and `verify_report.json` before claiming parity. |
| SRT output | **NVENC loopback verified** | `srt_output_v1`: a local FFmpeg caller decoded 90 live frames and exercised mid-GOP join | Test Intel/oneVPL, phone VLC over LAN, firewall onboarding, reconnect under packet loss, and non-local latency. Version 1 remains single-viewer and video-only. |
| Studio UI | **Self-test verified / interaction coverage partial** | `--ui-smoke`, `--ui-selftest`, and the optional AI leg passed on the development machine | Exercise panic during a live session, resize, exit while running, settings lockout, and corrupt/unreadable model UX. |
| Portable package | **Historical engine-only package verified; current package absent** | 2026-07-03 staged zip decoded SRT frames and passed UI smoke | Rebuild from current HEAD, exercise `--with-model`, test a clean Intel-only machine, and decide signing/SmartScreen/installer policy. |
| ONNX setup and classifier | **Runtime plumbing verified / accuracy unverified** | Real captured pixels reached ONNX Runtime with DirectML or CPU fallback; output varied; latency gates passed | Execute a clean dependency/model setup, label a real-screen sweep, measure accuracy, make the Stage A-to-B decision, and cover all five visual outcomes. |
| Policy-to-product action | **Advisory only** | `PolicyEngine` writes policy/trace; an opt-in full-screen demo action exists | `encode_hint` does not drive NVENC, oneVPL, or VideoToolbox. Do not market the demo as automatic content-aware encoding. |
| macOS phase 1/2 | **Historically verified / not current-host verified** | May artifacts cover ScreenCaptureKit, VideoToolbox, AVAssetWriter, chat-window producer, and CoreImage/Metal mosaic | Build and run the current tree on macOS; close capture-timestamp diagnostic and ultimate mask-failure semantic parity. |
| macOS secure text and ROI | **Stub or unsupported** | Explicit secure-text stub; trace reports ROI unsupported | Implement AXUIElement scanning off-thread; decide whether pixel-domain steering is useful when VideoToolbox lacks first-class per-rect H.264 QP. |
| Android | **Not implemented** | Reserved architecture paths only | Acquire appropriate hardware and approve the platform gate before creating MediaProjection, MediaCodec, LiteRT, JNI, or Gradle paths. |
| Agent adapter parity | **Static parity verified** | Seven role filenames in each GitHub, Claude, and Codex surface; Codex TOML parses; two architecture-sync skills exist | Run the same delegated task natively in all three hosts. Scheduled loops are definitions, not proven installed automation. |
| ROI benchmark governance | **Decision required** | Verifier runs `tools/roi_benchmark.ps1` and records process status | Decide whether adverse quality-direction text is a hard failure. Current pass/fail follows process exit. |
| Mask-stage cost | **Partially measured** | CPU-side dispatch timing remained healthy in historical runs | Add D3D11/Metal GPU timestamps before claiming complete shader-stage cost is under 4 ms. |
| Repository publication controls | **Files present / external settings unverified** | MIT license, contributing/security policies, least-privilege workflow permissions | Confirm public visibility, hosted CI, rendered links, branch/release settings, and private vulnerability reporting. |
| Formal release artifact | **Not created** | No tag or GitHub Release is claimed | Choose a version, rebuild current binaries, record checksums, decide signing/support scope, then create the release. |

## Troubleshooting Index

1. [Solid green video: incompatible plane copy failed silently](#1-solid-green-video-incompatible-plane-copy-failed-silently)
2. [Red and blue swapped: HLSL swizzle on BGRA textures](#2-red-and-blue-swapped-hlsl-swizzle-on-bgra-textures)
3. [Horizontal stripes: GPU row-pitch alignment](#3-horizontal-stripes-gpu-row-pitch-alignment)
4. [Periodic ROI flicker: GOP and I-frame interaction](#4-periodic-roi-flicker-gop-and-i-frame-interaction)
5. [ROI has no visible contrast: rate-control competition](#5-roi-has-no-visible-contrast-rate-control-competition)
6. [Intel CBR bitrate floor: effective configuration differs](#6-intel-cbr-bitrate-floor-effective-configuration-differs)
7. [Intel CQP failure with video-memory input](#7-intel-cqp-failure-with-video-memory-input)
8. [VPL initialization failure after Query negotiation](#8-vpl-initialization-failure-after-query-negotiation)
9. [B-frame crash: insufficient surface-pool depth](#9-b-frame-crash-insufficient-surface-pool-depth)
10. [Zero-copy with an explicit safe fallback](#10-zero-copy-with-an-explicit-safe-fallback)
11. [Non-blocking NVENC bitstream drain](#11-non-blocking-nvenc-bitstream-drain)
12. [Cross-engine synchronization with D3D11_QUERY_EVENT](#12-cross-engine-synchronization-with-d3d11_query_event)

## 1. Solid Green Video: Incompatible Plane Copy Failed Silently

### Symptom

The encoded H.264 output was a uniform green frame rather than a color cast or
partially corrupted image.

### Root cause

The retired compute-shader converter produced separate `R8_UNORM` Y and
`R8G8_UNORM` UV textures, then attempted to copy them into NV12 subresources:

```cpp
context->CopySubresourceRegion(nv12, 0, 0, 0, 0, yTexture, 0, nullptr);
context->CopySubresourceRegion(nv12, 1, 0, 0, 0, uvTexture, 0, nullptr);
```

Those source and destination formats were not copy-compatible. The driver path
completed without a useful failure signal, leaving the NV12 resource at zero.
An all-zero NV12 image converts to the characteristic green output.

### Resolution

Replace the custom plane-copy path with D3D11 `VideoProcessorBlt`, which performs
driver-supported BGRA-to-NV12 conversion and scaling in one operation.

### Engineering takeaway

When a media frame has a uniform, mathematically recognizable color, derive the
expected YUV value before debugging higher layers. A successful API call does
not prove a format-incompatible copy changed the destination.

## 2. Red and Blue Swapped: HLSL Swizzle on BGRA Textures

### Symptom

Red objects appeared blue and skin tones shifted toward blue.

### Root cause

WGC surfaces use `DXGI_FORMAT_B8G8R8A8_UNORM`. HLSL component selectors expose
logical color components after the texture format mapping. Writing
`sample.bgr` performed a second swap; `.r` was already the visual red channel,
not simply the first byte in memory.

### Resolution

```hlsl
float3 rgb = sample.rgb;
```

### Engineering takeaway

Separate physical byte layout from shader-visible logical components. Use
controlled test colors or `.xyzw` experiments when component interpretation is
uncertain.

## 3. Horizontal Stripes: GPU Row-Pitch Alignment

### Symptom

The frame appeared sliced into horizontal bands whose rows drifted sideways.

### Root cause

Mapped GPU rows are padded for cache/DMA alignment. The physical stride is
`D3D11_MAPPED_SUBRESOURCE.RowPitch`, not `width * bytesPerPixel`. Advancing the
source pointer by logical width accumulated one padding error per row.

### Resolution

```cpp
for (int row = 0; row < height; ++row) {
    memcpy(dst + row * destinationStride,
           source + row * mapped.RowPitch,
           logicalRowBytes);
}
```

### Engineering takeaway

Logical dimensions and physical layout are different in every major graphics
API. Always use the mapped row/slice pitch returned by the driver.

## 4. Periodic ROI Flicker: GOP and I-Frame Interaction

### Symptom

With ROI enabled, the image cycled between sharp and soft at a fixed interval.
Shorter GOPs made the cycle more obvious.

### Root cause

ROI applies macroblock QP deltas, but an I-frame starts an independent rate-
control decision. With `GopPicSize=2`, half the frames are expensive I-frames;
the ROI contrast repeatedly resets while the bitrate budget is consumed by
intra frames.

### Resolution

Use a GOP closer to half a second or one second for the intended latency and
seek behavior. Avoid extremely small GOP sizes in an ROI visual comparison.

### Engineering takeaway

Codec settings interact. ROI, GOP structure, and rate control cannot be tuned
independently because they compete for the same bit budget and temporal state.

## 5. ROI Has No Visible Contrast: Rate-Control Competition

### Symptom

The ROI and background looked similar even with a large negative QP delta.

### Root cause

ROI is bit redistribution. Under CBR, improving the ROI removes budget from the
background, making contrast visible. Under VBR, the encoder may increase total
bits and preserve the background as well. High bitrates reduce competition and
therefore hide the effect.

### Resolution

For an ROI comparison, use CBR, intentionally constrained bitrate, a meaningful
negative delta, and a reasonably small region. Treat this as an engineering
experiment, not the Live Share Guard product path.

### Engineering takeaway

If ROI contrast is absent, inspect rate-control mode and available budget before
assuming the region map is broken.

## 6. Intel CBR Bitrate Floor: Effective Configuration Differs

### Symptom

A requested 600 kbps CBR configuration produced roughly 1074 kbps at 1080p30.

### Root cause

The tested Intel driver enforced a minimum bitrate and returned
`MFX_WRN_INCOMPATIBLE_VIDEO_PARAM` from `MFXVideoENCODE_Query`. The negotiated
parameters differed from the request; careless structure replacement could also
lose the original target value.

### Resolution

Run Query, adopt the negotiated parameters, retain the original request for
comparison, and print requested versus effective bitrate. Read final values
through `GetVideoParam`, including `BRCParamMultiplier`.

### Engineering takeaway

Configuration is not proof of effective configuration. Production logs should
show both and flag a mismatch.

## 7. Intel CQP Failure with Video-Memory Input

### Symptom

Switching to `MFX_RATECONTROL_CQP` caused the first `EncodeFrameAsync` call to
return `MFX_ERR_DEVICE_FAILED` on the tested Intel UHD path.

### Root cause

The tested driver/GPU combination did not support CQP with
`MFX_IOPATTERN_IN_VIDEO_MEMORY`, although a CPU-memory path could work.

### Resolution

Keep CBR for the zero-copy path and record the failed combination so later work
does not repeat the experiment without new hardware/driver evidence.

### Engineering takeaway

Negative results are part of the compatibility matrix. Record the exact
hardware, memory model, and rate-control combination rather than declaring a
codec mode universally unsupported.

## 8. VPL Initialization Failure after Query Negotiation

### Symptom

`MFXVideoENCODE_Init` returned `MFX_ERR_INVALID_VIDEO_PARAM` even though the
requested structure appeared reasonable.

### Root cause

`MFXVideoENCODE_Query` validates and negotiates. When it returns adjusted
parameters, initializing with the original structure violates the driver's
accepted contract.

### Resolution

Use the required order:

```text
MFXLoad
-> MFXCreateSession
-> create D3D11 device
-> MFXVideoCORE_SetHandle
-> MFXVideoENCODE_Query and adopt negotiated values
-> MFXVideoENCODE_Init
```

### Engineering takeaway

Treat Query as capability negotiation, not an optional diagnostic call.

## 9. B-Frame Crash: Insufficient Surface-Pool Depth

### Symptom

Enabling `GopRefDist=3` caused a crash with the legacy three-surface pool.

### Root cause

B-frames need both past and future references, increasing the number of
simultaneously retained surfaces. The pool could not satisfy the dependency.

### Resolution

The low-latency path uses `GopRefDist=1`. Enabling B-frames requires a larger
pool, buffer recalculation, and an explicit latency/quality gate.

### Engineering takeaway

B-frame compression efficiency costs lookahead, memory, and latency. Codec
structure must follow the product latency budget.

## 10. Zero-Copy with an Explicit Safe Fallback

### Symptom

Some Intel driver/GPU combinations failed to create the desired D3D11 NV12
video-memory texture bindings.

### Design

When GPU zero-copy initialization fails, oneVPL can fall back to a CPU-copy path
instead of making the product unusable. The fallback must be logged, sticky for
the session, and included in the final report; retrying a known failure every
frame would add instability.

### Engineering takeaway

Graceful degradation is safe only when the boundary is visible. Operators must
know which path completed the run and what performance was sacrificed.

## 11. Non-Blocking NVENC Bitstream Drain

### Problem

NVENC submission is asynchronous. Locking an output buffer on the producer
thread before the frame is ready turns the pipeline back into a synchronous
encoder.

### Design

A background drain thread scans in-flight slots with non-blocking lock behavior,
releases completed input slots, and notifies producers through a condition
variable. It backs off instead of busy-spinning. A separate writer thread owns
disk I/O so bitstream writes never hold the pipeline state mutex.

### Engineering takeaway

Asynchronous hardware requires explicit ownership of submission, completion,
backpressure, and shutdown. Polling policy and disk I/O are part of latency
correctness, not incidental implementation detail.

## 12. Cross-Engine Synchronization with D3D11_QUERY_EVENT

### Problem

D3D11 copy/3D work and the hardware video-encode engine are separate execution
domains. `Flush()` submits commands but does not wait for completion. Encoding
too early can consume stale or partially updated NV12 data.

### Resolution

End a `D3D11_QUERY_EVENT` after the copy, flush the queue, and poll `GetData`
until the event completes before submitting the surface to the dependent engine.

### Engineering takeaway

Queue submission is not synchronization. Cross-engine resource handoff needs a
real completion primitive whose placement matches the producer/consumer
dependency.

## Decision Summary

| Decision | Reason |
|---|---|
| CBR for ROI comparison | ROI contrast requires a constrained, shared bit budget. |
| `GopRefDist=1` by default | Low latency is more important than B-frame compression gain. |
| `VideoProcessorBlt` | Uses the driver-supported BGRA-to-NV12 path and avoids incompatible plane copies. |
| Encoder-owned NV12 pool | Producer writes directly into registered encoder input textures. |
| Background drain and writer threads | Preserve non-blocking submission and keep disk I/O outside pipeline locks. |
| Scene-first decision layer | ROI and masking are downstream actions; content state is the primary decision. |
| File-based agent artifacts | Reproducible evidence is more reliable than conversational memory. |

## Investigation Depth Rule

This is not a catalog of generic errors. Each retained item has a specific
symptom, a root cause tied to this pipeline, a verified or deliberate
resolution, and a reusable engineering lesson. Add a new story only when that
standard is met.

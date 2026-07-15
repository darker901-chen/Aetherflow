# macOS Agent Verification Gate

This document defines the macOS agent verification gate.

Current status:

```text
macOS phase 1 (capture + encode + trace) landed and passed verification on 2026-05-10.
Run dir: .aetherflow/runs/macos_capture_encode_phase1/
Evidence: verify_report.json (status=passed; build/smoke/trace gates all passed).

macOS phase 2 chat-window mosaic (MacosNotificationProducerModule +
MacosPrivacyMaskCompositor via CoreImage + Metal) landed and passed verification
on 2026-05-11 (passthrough regression) and 2026-05-12 (mask-positive).
Run dirs:
  - .aetherflow/runs/mac_chat_window_mosaic/ (passthrough regression guard,
    privacy_mask_total=0, privacy_mask_paths={"none": 500})
  - .aetherflow/runs/mac_chat_window_mosaic_masked/ (mask-positive with LINE /
    Microsoft Teams visible, privacy_mask_total=5500, privacy_mask_applied=5500,
    privacy_mask_paths={"coreimage-bgra-mosaic": 500}, 0 fallback frames,
    steady-state mask_ms mean 5.40 ms / p99 7.14 ms at 11 rect/frame).
Evidence: verify_report.json (status=passed; build/smoke/trace gates all passed)
on both runs.

Phase 2 sub-phase still open: AXUIElement secure-text-field detection (still
a no-op stub). The Windows `Right Ctrl` latched hotkey is a dev/test
affordance, not a product feature, so macOS does not plan a parity hotkey.
Phase 3 (ROI/QP) is not implemented (`roi_supported=false`).
```

## Goal

The same agent workflow applies on macOS:

```text
Observe -> Architecture Planning Gate -> Reproduce -> Classify -> Patch
-> Build -> Smoke Test -> Benchmark -> Judge -> Report
```

The macOS gate currently verifies:

- ScreenCaptureKit capture initializes
- screen recording permission state is reported
- frames are captured
- frames reach the encoder input path
- VideoToolbox H.264 encode produces output
- AVAssetWriter passthrough mux produces an MP4 container
- frame trace fields are present (including `platform="macos"` and `roi_supported=false`)
- no unsupported Windows-only path is used

## Related Documentation

The docs directory map lives in
[`docs/DOCUMENTATION_INDEX.md`](../DOCUMENTATION_INDEX.md). Keep this file focused
on macOS verification behavior and gate criteria.

## Current Tool Behavior

`tools/agent_run.py` supports:

```bash
python3 tools/agent_run.py --platform auto
python3 tools/agent_run.py --platform windows
python3 tools/agent_run.py --platform macos
```

For `--platform windows`, current behavior is unchanged.

For `--platform macos`, the tool now performs a real CMake build of the
macOS target and runs the ScreenCaptureKit / VideoToolbox / AVAssetWriter
pipeline through `MacosPlatformShim::RunMacosPipeline`. Per-run artifacts:

- `run_manifest.json`
- `platform_status.json`
- `console.log`
- `build.log`
- `macos_smoke.json` (capture/encode counters, duration, output path, permission state)
- `frame_trace.jsonl` (Windows-aligned schema; one line per frame)
- `artifacts/output/output.mp4` (AVAssetWriter passthrough container)

The tool classifies the run honestly:

- `passed` when build, capture, and encode all succeeded.
- `unsupported` when Screen Recording permission is missing (the parent terminal
  or app launching `build/AetherFlow` is not in System Settings -> Privacy &
  Security -> Screen Recording).
- `failed` when build or runtime errored.
- `scaffolded` only when both `--skip-build` and `--skip-run` are passed so
  agents can produce planning artifacts.

`tools/agent_verify.py` supports:

```bash
python3 tools/agent_verify.py --platform auto --run-dir .aetherflow/runs/<run_id>
python3 tools/agent_verify.py --platform windows --run-dir .aetherflow/runs/<run_id>
python3 tools/agent_verify.py --platform macos --run-dir .aetherflow/runs/<run_id>
```

For `--platform windows`, current build / trace verification remains active.

For `--platform macos`, the verifier now writes `verify_report.json` with
real gates:

- `build` gate from `platform_status.json` and `build.log`.
- `smoke` gate from `macos_smoke.json` (permission granted, captured > 0,
  encoded > 0, output file size > 0).
- `trace` gate from `trace.summary.json` (parse_errors=0, frames > 0,
  encoded_frames > 0, plus the new `platform` and `roi_supported` keys).
- privacy-mask gates added with phase 2 (`verify_macos`):
  1. `privacy_mask_total > 0` requires `applied == total` (every emitted mask
     was applied before encode).
  2. `privacy_mask_fallback_frames == 0` (no `coreimage-bgra-clearfill-fallback`
     in mask-positive runs).
  3. `coreimage-bgra-clearfill-fallback` must not appear in `privacy_mask_paths`
     with a positive count.
  4. When `privacy_mask_total == 0`, `privacy_mask_paths` must be `{"none":
     frames}`-only (this is the passthrough regression guard exercised by the
     `mac_chat_window_mosaic` canonical run).

The phase-2 macOS compositor advertises trace path labels
`coreimage-bgra-clearfill`, `coreimage-bgra-blur`, `coreimage-bgra-mosaic`, and
the fail-closed `coreimage-bgra-clearfill-fallback`.

`--run-benchmark` is a no-op warning on macOS phase 1 because
`roi_supported=false` and the ROI benchmark (`tools/roi_benchmark.ps1`) is
Windows-only (NVENC / VPL).

`tools/agent_summarize.py` adds two new top-level keys to `trace.summary.json`
on every run: `platform` (string, e.g. `"macos"` / `"windows"`) and
`roi_supported` (bool). Windows summary content is otherwise unchanged.

## Verified Command Sequence

The bash command sequence that produced the passing phase-2 chat-window mosaic
artifact on 2026-05-11:

```bash
python3 tools/agent_run.py --platform macos --run-id mac_chat_window_mosaic
python3 tools/agent_verify.py --platform macos --run-dir .aetherflow/runs/mac_chat_window_mosaic
```

The phase-1 capture-and-encode command sequence (verified 2026-05-10) was:

```bash
python3 tools/agent_run.py --platform macos --run-id macos_capture_encode_phase1
python3 tools/agent_verify.py --platform macos --run-dir .aetherflow/runs/macos_capture_encode_phase1
```

Manual prerequisite: System Settings -> Privacy & Security -> Screen Recording
must include the parent terminal/app launching `build/AetherFlow`. If the
permission is not granted, `agent_run.py` honestly returns `status="unsupported"`
(it is not reported as a failure, because the runtime itself never executed).

## macOS Runtime Artifacts

A passing macOS smoke produces at minimum:

```text
.aetherflow/runs/<run_id>/
  run_manifest.json
  platform_status.json
  build.log
  console.log
  macos_smoke.json
  frame_trace.jsonl
  trace.summary.json
  verify_report.json
  artifacts/output/output.mp4
```

`macos_smoke.json` schema (verified on `macos_capture_encode_phase1`):

```json
{
  "schema_version": 1,
  "platform": "macos",
  "screen_capture_permission": "granted",
  "capture_backend": "ScreenCaptureKit",
  "encoder_backend": "VideoToolbox",
  "captured_frames": 900,
  "encoded_frames": 900,
  "encode_failure_frames": 0,
  "duration_seconds": 31.614,
  "output_path": ".aetherflow/runs/<run_id>/artifacts/output/output.mp4",
  "errors": []
}
```

## Pass Criteria For The macOS Gate

| Criterion | Current Evidence Status |
|---|---|
| Build passed for the macOS target | Met |
| ScreenCaptureKit permission granted (or explicitly skipped by a test mode) | Met (granted in canonical run) |
| Captured frames > 0 | Met (900) |
| Encoded frames > 0 | Met (900) |
| Encode failure frames = 0 | Met (0) |
| Trace summary exists | Met |
| Trace parse errors = 0 | Met (0) |
| Output container exists (`output.mp4`) | Met (3,019,295 bytes) |
| `trace.summary.json` carries `platform` and `roi_supported` | Met |
| Privacy mask compositor on Mac | Met historically for phase-2 chat-window mosaic (`mac_chat_window_mosaic` and `mac_chat_window_mosaic_masked`); current-tree macOS rerun remains open |
| AXUIElement secure-text-field detection | Not met (phase-2 sub-phase; module is a no-op stub) |
| Per-rect ROI / QP on Mac | Not met (phase 3; `roi_supported=false`) |

If a future run cannot meet the build/smoke/trace criteria, the correct
status is `unsupported` or `failed`, not `passed`.

## Relationship To Product Roadmap

macOS phase 1 (capture + encode + MP4 mux) landed after the Windows-first Live
Share Guard path proved:

```text
capture -> privacy mask -> encode -> trace
```

Mac phase 2 already added the CoreImage/Metal compositor and
`MacosNotificationProducerModule` chat-window masking. Its AXUIElement
secure-text-field sub-phase remains open. Mac phase 3 will explore ROI/QP
(note: VideoToolbox H.264 has no first-class per-rect QP; it would require
pixel-domain steering or HEVC tile-level QP).

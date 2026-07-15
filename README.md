# AetherFlow

A Windows-first, **GPU-resident fast-path** screen-capture → H.264 pipeline (NVIDIA NVENC /
Intel oneVPL) with a **deterministic, scene-first** privacy layer that masks
sensitive content **before** it is encoded — fully on-device, no cloud, no chat
LLM in the runtime.

- **Problem** — screen sharing leaks: a password field, a LINE / Teams popup, or
  a notification slips into the stream before you can react.
- **Approach** — a GPU-resident capture → scene-first decision → pre-encode mask
  → encode pipeline; deterministic rules decide what to hide, and an optional
  local ONNX scene classifier runs off-thread (advisory only).
- **Evidence** — behavior is verified locally through reproducible file-based
  run artifacts (`verify_report.json` + per-frame `frame_trace.jsonl`), produced
  by a self-built **7-role agent development workflow** with autonomous detect
  → repair → re-verify and a cross-run audit ledger. Raw run bundles are
  gitignored because traces can contain captured-screen-sensitive data; public
  current boundaries live in `PROJECT_STATUS.md`, dated product evidence lives
  in `VERIFICATION_HISTORY.md`, and agent-workflow catches live in the
  effectiveness log.

![AetherFlow Studio settings for deterministic privacy masks and SRT streaming](docs/assets/aetherflow-studio-ui.png)

*AetherFlow Studio: deterministic UI Automation password-field and recognized
messenger-window masks are applied before encoding; SRT streams the already-masked
output. The ONNX scene classifier is optional and advisory.*

The load-bearing engineering is threefold: **(i)** the self-built 7-role agent
development protocol ([AGENT_ARCHITECTURE.md](docs/2-agent-system/AGENT_ARCHITECTURE.md)
· [AGENT_EFFECTIVENESS_LOG.md](docs/2-agent-system/AGENT_EFFECTIVENESS_LOG.md)),
**(ii)** cross-platform native GPU media craft (Windows WGC + D3D11 + NVENC /
oneVPL; macOS ScreenCaptureKit + VideoToolbox), and **(iii)** a deterministic
scene-first runtime with frame-level trace schema v3, verifier-gated handoff, and
a longitudinal audit ledger.

> **Platform status.** Windows is the primary verified path. macOS phase 1
> (ScreenCaptureKit + VideoToolbox + AVAssetWriter MP4) and phase 2 chat-window
> mosaic (`MacosNotificationProducerModule` + `MacosPrivacyMaskCompositor`) are
> historically verified in dated runs but were not rerun on the current Windows
> host; open: AXUIElement secure-text-field producer (phase-2 sub-phase) and
> phase 3 (ROI/QP). Full status →
> [PROJECT_STATUS.md](docs/1-status/PROJECT_STATUS.md).

> **Release status (2026-07-14).** This repository is a **pre-release source
> snapshot**: no formal version, Git tag, or GitHub Release is claimed. The current Windows
> source/build smoke and four first-party tests pass locally, but that does not
> mean every optional feature, backend, or platform was re-verified at current
> HEAD. The last portable zip was self-tested on 2026-07-03 and has **not** been
> rebuilt from the current public-hardened tree; it is unsigned and is not a
> current release artifact. Exact implemented / verified / unverified coverage
> is maintained in [PROJECT_STATUS.md](docs/1-status/PROJECT_STATUS.md) and the
> [QA coverage registry](docs/4-qa-debugging/TROUBLESHOOTING_QA.md#qa-coverage-registry-2026-07-14).

## Documentation

Start here:

- [docs/HIGHLIGHTS.md](docs/HIGHLIGHTS.md) — the 60-second skim: pitch, two-lane
  architecture, what carries the weight, measurable results.
- [docs/DOCUMENTATION_INDEX.md](docs/DOCUMENTATION_INDEX.md) — map of every
  project document and the canonical owner for repeated topics.
- [docs/1-status/PROJECT_STATUS.md](docs/1-status/PROJECT_STATUS.md) — current capability,
  verification boundary, residual risks, and next checks.
- [docs/4-qa-debugging/VERIFICATION_HISTORY.md](docs/4-qa-debugging/VERIFICATION_HISTORY.md)
  — dated run IDs, measurements, and historical review outcomes.
- [docs/3-product/ARCHITECTURE.md](docs/3-product/ARCHITECTURE.md) — durable runtime and product
  architecture.
- [docs/OPERATION_GUIDE.md](docs/OPERATION_GUIDE.md) — **detailed operation manual**: every
  feature in depth, all flags / env vars, human-verification SOPs, full macOS
  setup, ONNX Runtime install.
- [docs/2-agent-system/AGENT_ARCHITECTURE.md](docs/2-agent-system/AGENT_ARCHITECTURE.md) — how the seven
  development-agent responsibilities cooperate;
  [AGENT_EFFECTIVENESS_LOG.md](docs/2-agent-system/AGENT_EFFECTIVENESS_LOG.md) is the cited record of
  where that workflow caught / diagnosed / measured something real.
- [docs/4-qa-debugging/TROUBLESHOOTING_QA.md](docs/4-qa-debugging/TROUBLESHOOTING_QA.md) — current QA
  coverage and unverified matrix, followed by root-caused debugging stories.

Agent entry contracts: [AGENTS.md](AGENTS.md) (Codex / file-based agents) ·
[CLAUDE.md](CLAUDE.md) (Claude Code) ·
[protocol/COMPONENT_INDEX.md](protocol/COMPONENT_INDEX.md) (locate owner files).

Repository policy: [CONTRIBUTING.md](CONTRIBUTING.md) ·
[SECURITY.md](SECURITY.md).

## Agent Workflow

For implementation work, one clear sentence is enough. Example:

```text
Implement a QR-code privacy-mask producer; do not add a remote AI detector. I approve starting under the repository agent workflow.
```

Codex and Claude then follow the shared repo workflow:

```text
Architecture Planning Gate -> owning implementation role -> agent_run
-> agent_verify -> benchmark when required -> one scoped repair attempt if a
gate fails -> independent Code Review when code changed -> docs/status sync
-> final artifact report
```

## Requirements

- Windows 10 1903+ / Windows 11
- Intel Gen 6 Skylake+ or NVIDIA Maxwell+ GPU
- Visual Studio or Build Tools 2019/2022 with **Desktop development with C++** and a Windows SDK
- CMake 3.20+; Python 3 for the default Studio dependency bootstrap

See [docs/3-product/ARCHITECTURE.md](docs/3-product/ARCHITECTURE.md) for the full runtime design.

## Quick Start (Windows)

The recommended path builds the settings UI, SRT streaming support, first-party
tests, and the deterministic privacy-mask pipeline. It deliberately leaves the
large optional ONNX classifier disabled.

```powershell
# Run from the repository root in PowerShell.
Set-ExecutionPolicy -Scope Process Bypass -Force
.\tools\bootstrap_windows.ps1

# Start the settings-window product after the bootstrap passes.
.\build\Release\AetherFlowStudio.exe
```

For prerequisites, a Core-only profile, NVENC setup, exact success criteria,
common failures, and the coding-agent contract, read
**[Build AetherFlow from a Fresh Windows Clone](docs/BUILD_WINDOWS.md)**.

For macOS, see the [Operation Guide § macOS](docs/OPERATION_GUIDE.md#macos).

```bash
# Manual core build (VS 2022 shown; for VS 2019 use -G "Visual Studio 16 2019")
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target AetherFlow

# Easiest — one-shot runners (build + run; the *_test scripts also verify):
./demo.sh                # product only: deterministic masks, no AI effect (Git Bash; ./demo.ps1 for PowerShell)
./run_full_test.sh       # all features + verify (AI runs only if set up — see "Fresh clone" note)
./run_full_test.sh --ai-cpu   # run the classifier on CPU instead of GPU

# Or run the binary directly. v0.2 defaults turn every product detector on
# (password-field UIA + chat-app whitelist, blur mode), so no flags are required:
./build/Release/AetherFlow.exe

# Common opt-outs / triggers (full reference in the Operation Guide):
./build/Release/AetherFlow.exe --no-password-field-mask
./build/Release/AetherFlow.exe --no-notification-mask
./build/Release/AetherFlow.exe --privacy-mask-mode=blackout    # blackout | blur | mosaic
./build/Release/AetherFlow.exe --panic-mask                    # mask the whole screen at startup
./build/Release/AetherFlow.exe --cursor-roi                    # opt-in mouse-tracking ROI QP boost (default OFF)

# SRT live streaming (watch the masked stream on any LAN device; needs the
# one-time `python tools/fetch_ffmpeg.py` + reconfigure — see Operation Guide):
./build/Release/AetherFlow.exe --srt-output    # prints srt://<your-LAN-IP>:8888 to paste into VLC/ffplay

# Settings-window version of the same pipeline — zero command line
# (the default bootstrap fetches ImGui and FFmpeg/SRT):
./build/Release/AetherFlowStudio.exe

# Portable zip for another machine (exes + all runtime DLLs + README):
python tools/package_portable.py               # -> output/AetherFlow-portable-<date>.zip

# Canonical agent harness (build + smoke + trace)
python tools/agent_run.py --run-id smoke
python tools/agent_verify.py --run-dir .aetherflow/runs/smoke
```

Outputs land in `output/`: NVENC `output_encoded.h264` (disable with
`AETHERFLOW_NVENC_WRITE_BITSTREAM=0`), oneVPL `output.mp4`. `demo.sh` / `demo.ps1`
set `AETHERFLOW_OUTPUT_DIR` to `<repo>/output` and mux `output/demo.mp4` via
ffmpeg if it is on PATH.

> **Fresh-clone boundary:** the bootstrap fetches only the pinned ImGui and
> FFmpeg/SRT packages needed for the showcased Studio path. oneVPL is already
> vendored. NVENC still needs one NVIDIA SDK header supplied by the user; ONNX
> Runtime and the model remain manual, optional, and advisory. Build success
> does not by itself prove that the machine has a supported encoder GPU/driver.

> Full flag/env reference, timed recording, visual modes, run-folder anatomy,
> and verify-vs-benchmark → **[Operation Guide](docs/OPERATION_GUIDE.md)**.

## Features at a Glance

The one distinction that trips everyone up — **deterministic masks (the real
product) vs. the AI demo effect (a toy)**:

| | Deterministic mask (real product) | AI demo effect (toy) |
|---|---|---|
| Decided by | Hard rules (password field / chat-app window detected) | AI's whole-screen scene guess |
| Coverage | **Only that window / region** | **Full screen** |
| Default | **On** | **Off** (needs `--scene-classifier-demo-action`) |
| Ships? | Yes, real feature | No, just a "pretend the AI drives masking" preview |

If you only want "mask the LINE window, not the whole screen," **do not pass
`--scene-classifier-demo-action`** — that applies a full-screen effect from the
AI's guess and is for demos only.

Which runner does what:

| I want to… | Command |
|---|---|
| All features once + verify + drop video & scene log into `output/` | `./run_full_test.sh` |
| Only real product behavior (mask sensitive windows, no full-screen AI effect) | `./demo.sh` |
| Check whether the AI guesses correctly (plain-language report) | `./run_scene_test.sh --demo` |

Full feature reference (deterministic detectors, the 5-class AI scene classifier,
visual modes, run-folder anatomy, verify-vs-benchmark) →
**[Operation Guide](docs/OPERATION_GUIDE.md)**.

## macOS

macOS phase 1 (ScreenCaptureKit + VideoToolbox + AVAssetWriter MP4) and phase 2
chat-window mosaic (CoreImage + Metal) are verified; the phase-2 sub-phase
(AXUIElement secure-text-field) and phase 3 (ROI/QP) are open. Quick start:

```bash
./demo.sh   # builds the macOS target, runs SCKit + VideoToolbox, writes output/demo.mp4
```

Full macOS setup, flags, run artifacts, and phase scope →
**[Operation Guide § macOS](docs/OPERATION_GUIDE.md#macos)**.

## Licenses

AetherFlow's own source is released under the **MIT License** ([LICENSE](LICENSE)).
Bundled / fetched third-party components keep their own licenses:

| Component | License |
|---|---|
| Intel oneVPL / libvpl | MIT |
| NVIDIA Video Codec SDK (NVENC header — fetched, not redistributed; see `external/VideoCodecSDK/SOURCE.md`) | NVIDIA SDK License |
| FFmpeg shared SDK (avformat/avcodec/avutil/swresample for SRT live output — fetched, not redistributed; see `third_party/ffmpeg/SOURCE.md`) | LGPL 2.1+ (BtbN lgpl-shared build, dynamic linking) |
| CMake | BSD |

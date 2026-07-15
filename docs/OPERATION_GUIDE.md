# AetherFlow — Operation Guide

The detailed operation manual: every feature in depth, all flags and env vars,
human-verification SOPs, the full macOS setup, and the optional ONNX Runtime
install. For a deterministic fresh-clone Windows build, start with
[BUILD_WINDOWS.md](BUILD_WINDOWS.md). For the 30-second pitch and quick start,
see [README.md](../README.md); for the strict runtime design see
[ARCHITECTURE.md](3-product/ARCHITECTURE.md).

> Plain-language "what each feature does and which command to run." Verified
> against `src/main.cpp` and `include/AetherFlow/SceneDemoActionModule.h`
> (last checked 2026-05-23; SRT section verified against `src/main.cpp` and
> `src/streaming/SrtStreamOutput.cpp` plus run `srt_output_v1`, 2026-07-03).

## Contents

- [Deterministic detectors (fast, local rules)](#deterministic-detectors-fast-local-rules)
- [AI scene classification (slow, observe-only by default)](#ai-scene-classification-slow-observe-only-by-default)
- [Visual mask modes](#visual-mask-modes)
- [SRT live streaming (watch on another device)](#srt-live-streaming-watch-on-another-device)
- [Studio UI (settings window, zero command line)](#studio-ui-settings-window-zero-command-line)
- [Portable package (zip for other machines)](#portable-package-zip-for-other-machines)
- [Timed recording (Windows, opt-in)](#timed-recording-windows-opt-in)
- [What is in a run folder](#what-is-in-a-run-folder)
- ["Verify" vs "benchmark"](#verify-vs-benchmark-verify-is-not-a-quality-score)
- [Password-field mask — human verification](#password-field-mask--human-verification)
- [Notification mask — human verification](#notification-mask--human-verification)
- [macOS](#macos)

---

## Deterministic detectors (fast, local rules)

`src/main.cpp` registers these four privacy-mask sources:

| Detector | Masks | Default | Toggle |
|---|---|---|---|
| Password-field mask | Windows UIAutomation password inputs (`IsPassword=true`) | **On** | `--no-password-field-mask` |
| Chat-app window mask | Visible windows of whitelisted apps (below) | **On** | `--no-notification-mask`; `--notification-mask-process=X` to set the list |
| Panic full-screen mask | The whole screen | **Off** | `--panic-mask` or `AETHERFLOW_PRIVACY_PANIC_MASK=1` (mask at startup), or press **Right Ctrl** at runtime (`AETHERFLOW_PRIVACY_PANIC_HOTKEY`, Windows dev/test hotkey, on by default) |
| Manual region | A rectangle you specify | **Off** | `--privacy-mask=left,top,right,bottom`, or `AETHERFLOW_PRIVACY_MASKS="left,top,right,bottom;..."` |

Chat-app whitelist (Windows, hard-coded in `main.cpp`): **LINE, Slack, Discord,
Teams** (incl. packaged `ms-teams` / `MSTeams`), **Telegram, WhatsApp**. A
detector being "on" does not mean something is masked — the window/field must
actually be present.

## AI scene classification (slow, observe-only by default)

A local ONNX model (CLIP zero-shot) sorts the screen into **5 classes**:
`code_text` (coding/text), `slides`, `video`, `mixed_ui` (generic desktop / web),
`sensitive_surface` (notifications / chat). Three states — this is where readers
get confused:

| State | How to trigger | What the AI does to the picture |
|---|---|---|
| Observe-only (**default**) | CLI `--scene-classifier-onnx-model=...`, or the Studio UI **AI scene detection** checkbox (unlocks when a model is found at launch), or nothing | **Nothing** — just writes its guess into the trace (Studio additionally shows it as a colored-dot status row) |
| Demo effect (toy) | Add `--scene-classifier-demo-action` (`run_full_test.sh` adds it; CLI-only, not in the Studio UI) | Applies a per-class effect to the **whole screen** (below) |
| Real AI-driven | — | **Not built yet**; planned as P1.1 (AI actually steering encode/mask) |

Demo-effect mapping (from `SceneDemoActionModule.h`): `sensitive_surface`→
Blackout, `video`→Mosaic, `code_text`→Blur, `slides`→Grayscale,
`mixed_ui` (generic desktop) / unknown→passthrough (desktop stays clean). If a
deterministic mask already fired on a frame, the demo effect yields — real masks
always win.

Honest notes: accuracy is **unverified** (zero-shot, no training; expect roughly
60–75%). `scene_classifier: passed` in the verify report only proves the AI saw
pixels and produced varying output — **not** that it guessed right. To see what
it guessed, read `output/scene_log.txt` or run `./run_scene_test.sh --demo`. It
runs on its own thread (~1 Hz) and does not block capture/encode.

## Visual mask modes

Any detector can render its mask as `blackout` (safest fallback), `blur`
(Windows default), or `mosaic` (macOS default), selected with
`--privacy-mask-mode=blackout|blur|mosaic`.
The AI demo effect additionally uses `grayscale` for `slides`.

## SRT live streaming (watch on another device)

Opt-in (default **off**). Streams the encoded output — with all privacy masks
already applied, since masking happens before encode — as MPEG-TS over an SRT
listener, so any device on the same LAN can watch live. Video-only, one viewer
at a time (v1); when the viewer disconnects, AetherFlow returns to listening.

The default Windows bootstrap already performs this setup. For a manual build,
the FFmpeg SDK is gitignored and can be fetched independently:

```bash
# Run from the repository root.
python tools/fetch_ffmpeg.py          # pinned LGPL build, ~75 MB download
cmake -S . -B build -G "Visual Studio 17 2022" -A x64   # reconfigure picks it up
cmake --build build --config Release --target AetherFlow
```

Start streaming (runs until you press **Ctrl+C** — the stop is clean: encoder
flush, performance report, and the `[SRT] summary` line still run; default
without the env var is 900 frames = 30 s):

```bash
AETHERFLOW_MAX_FRAMES=0 ./build/Release/AetherFlow.exe --srt-output   # 0 = no frame limit
```

The console prints the URLs to paste into the viewer:

```text
[SRT] local viewer URL: srt://127.0.0.1:8888
[SRT] LAN viewer URL:   srt://192.168.x.x:8888  (paste into VLC / ffplay on any LAN device)
```

Watch it: on a phone or second machine, open VLC → Network Stream → enter the
LAN URL (`srt://<host-ip>:8888`). On the same machine:
`ffplay srt://127.0.0.1:8888`. Expect the picture within ~2 s of connecting
(the stream starts at the next keyframe; GOP is 2 s).

| Flag / env | Default | Meaning |
|---|---|---|
| `--srt-output` / `AETHERFLOW_SRT_OUTPUT=1` | off | Enable the SRT listener |
| `--srt-port=N` / `AETHERFLOW_SRT_PORT` | 8888 | Listener port (`--srt-port=N` also implies enable) |
| `--srt-latency-ms=N` / `AETHERFLOW_SRT_LATENCY_MS` | 120 | SRT latency in ms |
| `--srt-passphrase=S` / `AETHERFLOW_SRT_PASSPHRASE` | empty | Optional AES-128 passphrase, **10–79 chars** (viewer must supply the same) |

Latency — what to expect and how to shrink it:

- End-to-end delay in VLC is typically **~1.2–1.5 s**, and most of it is
  **VLC's own network buffer**: VLC defaults to `network-caching=1000` (ms)
  for network streams. AetherFlow's side contributes only the SRT latency
  window (120 ms default) plus per-frame mux flush.
- Lower it on the viewer side — VLC: Media → Open Network Stream → Show more
  options → set Caching to 200–300 ms, or from a shell:

  ```bash
  vlc --network-caching=200 "srt://<host-ip>:8888"
  ```

  ffplay (lowest-latency check):

  ```bash
  ffplay -fflags nobuffer -flags low_delay -i "srt://<host-ip>:8888"
  ```

- Going below ~200 ms viewer caching on Wi-Fi trades smoothness for latency;
  the SRT latency window (`--srt-latency-ms`) is the sender-side floor.
- The first picture after connecting takes up to ~2 s regardless (the stream
  starts at the next keyframe; GOP is 2 s) — that is join delay, not
  steady-state lag.

Correct behavior and common wrong outcomes:

- Run-end console shows `[SRT] summary: connections=… sent=…` — `sent > 0`
  proves a viewer actually received the stream.
- **Ctrl+C** (or closing the console window) stops cleanly — you still get the
  performance report and `[SRT] summary`. A second Ctrl+C force-quits.
- **No picture from another device but `srt://127.0.0.1:8888` works locally** →
  Windows Firewall is blocking inbound UDP on the port; allow it for
  `AetherFlow.exe` (SRT runs over UDP).
- **`[SRT] --srt-output requested but this build was compiled without…`** →
  run the one-time setup above (fetch + reconfigure + rebuild).
- **Viewer connects but shows nothing and the screen is completely static** →
  Windows Graphics Capture only delivers frames when screen content changes;
  move a window or play something. (A fully idle desktop produces no new
  frames by design.)
- A second viewer while one is connected is refused (v1 single-viewer);
  fan-out is a later phase.

## Studio UI (settings window, zero command line)

`AetherFlowStudio.exe` is the same pipeline as `AetherFlow.exe` behind a
settings window — no command-line arguments needed at all. The default Windows
bootstrap builds and smoke-tests it. For a manual source build (the packaged
zip already includes everything):

```bash
# Run from the repository root.
python tools/fetch_imgui.py        # vendored UI toolkit (gitignored, ~1 MB)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target AetherFlowStudio
./build/Release/AetherFlowStudio.exe
```

What you get:

- **Capture & Encode** — monitor dropdown (multi-monitor supported), encoder
  Auto / NVENC / oneVPL (no CPU software encoder exists in this repo),
  resolution Native / 1080p / 720p, FPS 15/30/60, bitrate slider.
- **Privacy masks** — password-field / messenger-window toggles and the
  blackout / blur / mosaic style (same deterministic detectors as the CLI).
- **AI scene detection (advisory)** — a "Detect scene type (CLIP ONNX,
  DirectML)" checkbox. It only unlocks when the model file is found at launch,
  checked in this order: the `AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL` env var
  (authoritative when set — a bad path grays the toggle out rather than
  silently loading a different model), then `models\scene_classifier_v1.onnx`
  next to the exe (the `--with-model` package layout), then the dev-tree
  `models\` (source builds). No model ⇒ the checkbox renders grayed out with a
  hint; your saved preference is kept for when a model appears. Advisory only:
  the AI never changes masks, encoding, or the stream — it just reports what
  it sees (below).
- **SRT live stream** — port / latency / optional passphrase (10–79 chars;
  an invalid length disables Start with a red message instead of silently
  streaming unencrypted).
- **Start / Stop** — Start runs the pipeline until you press Stop (clean
  shutdown, same closeout as the CLI's Ctrl+C). Settings are saved to
  `aetherflow_studio.ini` beside the exe and restored on next launch.
- **Panic** — one click masks the whole stream (click again to clear);
  the global Right-Ctrl latch works too.
- **Status** — encoded frames, effective fps, "Masking now" with the source
  (password field / messenger window / panic / …), SRT viewer state and bytes
  sent, plus the viewer URLs with copy buttons. With AI scene detection on,
  an extra **AI scene row**: a per-class colored dot (green `code_text`, blue
  `slides`, purple `video`, yellow `mixed_ui`, red `sensitive_surface`) +
  `AI scene: <class> (NN%) via DirectML|CPU`. A gray dot with "no confident
  verdict yet..." means the classifier has not (currently) beaten the
  baseline — cold start or low confidence; `[deterministic override]` means a
  rule-based detector (password/messenger/panic/manual) out-ranked the AI this
  frame; an orange "AI: failed to start" line means the model could not load
  and the session continues without AI.
- **Recordings** — every session also writes the encoded video + trace under
  the `output` folder next to the exe (shown as "Recording to:" in the
  window); long sessions grow it ~2.7 GB/hour at 6 Mbps.

Headless gates for this target: `--ui-smoke` (renders 3 hidden frames, exit
0) and `--ui-selftest` (programmatically Starts a 60-frame session and
requires ≥30 encoded frames + clean stop — needs some screen activity, like
any capture run). When a scene-classifier model is on disk, the selftest adds
a second 150-frame leg with AI on and also requires the classifier to
initialize AND win the scene merge on at least one frame; with no model the
AI leg is skipped and the gate stays green.

## Portable package (zip for other machines)

```bash
python tools/package_portable.py               # output/AetherFlow-portable-<date>.zip
python tools/package_portable.py --with-model  # + the 335 MB CLIP model (AI demo offline)
```

Stages both exes, every runtime DLL (oneVPL, onnxruntime when built,
FFmpeg avformat/avcodec/avutil/swresample when built), app-local VC++ runtime
DLLs (no vc_redist install needed), license notices, and a plain-language
`README_PORTABLE.txt`. Before zipping it **runs the staged folder**: a
300-frame `--srt-output` session that an ffmpeg client must actually decode
from, plus `AetherFlowStudio.exe --ui-smoke` — a missing DLL fails the build
loudly. Unzip anywhere and double-click `AetherFlowStudio.exe`.

Honest limits: the exe is unsigned (SmartScreen will warn — "More info" →
"Run anyway"); DirectML.dll is not bundled (Windows 10 1903+ ships it);
a true clean-machine double-click test still needs a second machine.

## Timed recording (Windows, opt-in)

Set `AETHERFLOW_TIMED_RECORDING=1` (or pass `--record-timed`) and the encoder
additionally writes a per-frame PTS sidecar
`output/output_encoded.timestamps.txt` (mkvmerge timecodes v2, 1:1 with encoded
frames). `run_scene_test.sh` sets this automatically and muxes honoring real
capture time: with `mkvmerge` on PATH it produces a per-frame-exact
`scene_test_out/demo.mkv`; otherwise it falls back to ffmpeg muxing at the
**real average fps** computed from the sidecar (duration ≈ real capture span,
not `frames/30`). Default (flag unset, e.g. the canonical `tools/agent_*` verify
path) is byte-identical: raw `output_encoded.h264` only, no sidecar, fixed-rate
mux — verifier gates unaffected. On macOS, `./demo.sh` runs the agent harness
instead and uses AVAssetWriter's native MP4 output (no ffmpeg needed).

## What is in a run folder

Each `agent_run.py` / `run_full_test.sh` run leaves a bundle under
`.aetherflow/runs/<name>/`:

| File | What it is |
|---|---|
| `console.log` / `console.summary.json` | Everything the exe printed (incl. `Capture Failures: X / Y`) + a condensed view |
| `frame_trace.jsonl` | **Per-frame** truth: scene detected, what was masked, timings |
| `trace.summary.json` | The above aggregated into a few numbers |
| `verify_report.json` | **Whether this run passed** (see below) |
| `build.log` / `verify_build.log` | Compiler output |
| `run_manifest.json` | What command ran, where output went, git revision |
| `plan.md` / `task_card.json` / `handoff.md` / `diagnosis.json` | Dev-agent workflow files (unrelated to watching the video) |
| `artifacts/output/` | The actual encoded video + raw trace |

Run folders are scratch and can be deleted wholesale (`rm -rf .aetherflow/runs/*`).

## "Verify" vs "benchmark" (verify is not a quality score)

`verify_report.json` is a **health check** ("did this run work"), not a video-
quality report. It checks: **build** (compiled), **smoke** (exe produced a
trace), **trace** (all frames encoded, no errors, masks applied, capture-failure
ratio not too high), and **scene_classifier** (AI ran — accuracy is **not**
checked). Quality / bandwidth comparison is a separate **benchmark**, run only
with `--run-benchmark` or via `run_aetherflow.sh`. Quick mnemonic:

- `verify_report.json` = "did this run break"
- benchmark (opt-in) = "is quality/bandwidth better"
- `output/scene_log.txt` = "what the AI thought you were doing"
- `output/demo.mp4` = the actual recorded video

## Password-field mask — human verification

Use this when you want to verify the UIAutomation password-field privacy mask by
watching the captured video yourself.

```bash
# Run from the repository root.
cmake --build build --config Release --target AetherFlow

# Open the local password test page. Keep the browser on the main monitor.
explorer.exe "$(cygpath -w fixtures/password_field_fixture.html)"
```

Click the password input so the browser page is the foreground window, then run:

```bash
AETHERFLOW_NVENC_WRITE_BITSTREAM=1 ./build/Release/AetherFlow.exe --password-field-mask
ls -lh output/output_encoded.h264
explorer.exe "$(cygpath -w output)"
```

Correct visual result:

- The password input rectangle is blacked out in `output/output_encoded.h264`.
- The page title and normal text remain visible.
- The whole screen is not black; that would be the panic-mask path.
- If no black rectangle appears, click the password input again and rerun.

## Notification mask — human verification

Use the local fixture when you want to verify the deterministic visible
notification/popup window mask without opening a real messenger app.

```bash
# Run from the repository root.
cmake --build build --config Release --target AetherFlow
powershell.exe -NoProfile -ExecutionPolicy Bypass -File fixtures/notification_fixture.ps1
```

Keep the fixture window visible, then run this from a second shell:

```bash
AETHERFLOW_NVENC_WRITE_BITSTREAM=1 ./build/Release/AetherFlow.exe --notification-mask --notification-mask-process=powershell.exe
```

The producer masks visible top-level/popup windows when their executable leaf
filename is in the process whitelist, clipping masks against higher z-order
windows. Microsoft Teams entries also expand to the current packaged Teams
identity (`ms-teams.exe` / `MSTeams_*`) so the default is not tied to only the
classic `Teams.exe` leaf filename. Environment equivalents are
`AETHERFLOW_NOTIFICATION_MASK=1`, `AETHERFLOW_NOTIFICATION_PROCESS_LIST`, and
`AETHERFLOW_NOTIFICATION_MASK_POLL_FRAMES`.

## macOS

macOS phase 1 covers ScreenCaptureKit capture, VideoToolbox H.264 encode, and
AVAssetWriter passthrough mux to MP4. macOS phase 2 adds the chat-window
mosaic: `MacosNotificationProducerModule` (deterministic CGWindowList-driven
producer with a default whitelist) plus `MacosPrivacyMaskCompositor` (CoreImage
+ Metal, IOSurface-backed BGRA pool, `blackout` / `blur` / `mosaic` modes with
a fail-closed clear-fill fallback). Verified by two canonical runs:
`.aetherflow/runs/mac_chat_window_mosaic/` (2026-05-11; no-mask passthrough
regression guard) and `.aetherflow/runs/mac_chat_window_mosaic_masked/`
(2026-05-12; mask-positive with LINE / Microsoft Teams visible — 5500/5500
masks applied via `coreimage-bgra-mosaic`, 0 fallback frames, steady-state
mask_ms mean 5.40 ms / p99 7.14 ms). Both runs report
`verify_report.json status: passed`.

Phase-2 sub-phase (AXUIElement secure-text-field producer) and phase 3 (ROI/QP)
are still open. The Windows Right Ctrl latched hotkey is a dev/test affordance,
not a product feature; macOS does not need a parity hotkey.

Prerequisites:

- macOS 13.0 or newer (`CMAKE_OSX_DEPLOYMENT_TARGET=13.0`).
- Xcode Command Line Tools (`xcode-select --install`) or full Xcode.
- CMake 3.20+ (`brew install cmake`).
- System Settings -> Privacy & Security -> Screen Recording must include the
  parent terminal or app launching `build/AetherFlow`. If not granted,
  `agent_run.py` returns `status="unsupported"` (it is not reported as a
  failure, because the runtime never executed).

One-shot demo runner (recommended for the first run):

```bash
./demo.sh
# Builds the macOS target if needed, runs the ScreenCaptureKit + VideoToolbox
# pipeline through the agent harness (run id: `demo`), copies the resulting
# MP4 to `output/demo.mp4`, and opens the output folder via `open`.
```

Or the canonical agent commands directly:

```bash
python3 tools/agent_run.py --platform macos --run-id mac_chat_window_mosaic
python3 tools/agent_verify.py --platform macos --run-dir .aetherflow/runs/mac_chat_window_mosaic
```

On macOS, every `agent_run.py` run also publishes a byte-identical copy of the
mp4 to `output/demo.mp4` (recorded as `stable_output` in `run_manifest.json`),
regardless of `--run-id` — a stable, double-clickable path to eyeball without
digging into `.aetherflow/runs/<id>/artifacts/output/`. It is a copy, not a
move: the verifier still reads the per-run run-dir artifact. (On Windows
`agent_run.py` emits raw `output_encoded.h264`; the `output/demo.mp4` there
comes from `demo.sh`'s ffmpeg mux instead.)

### Phase-2 defaults and flag/env reference

The chat-window mosaic is **on by default** with visual mode `mosaic` and the
built-in 7-app whitelist (LINE / Slack / Discord / Microsoft Teams / Teams /
Telegram / WhatsApp / Messages). The default frame budget is **500 frames**
(lowered from phase-1 900 to match the Windows `AETHERFLOW_MAX_FRAMES`
fallback). The macOS binary parses these flags and env vars (most are
cross-platform; Windows-only ones are noted):

| Flag | Env var | Effect |
|---|---|---|
| `--notification-mask` / `--no-notification-mask` | `AETHERFLOW_NOTIFICATION_MASK` (default `1`) | Enable / disable the chat-window producer |
| `--notification-mask-process=<owner>` | `AETHERFLOW_NOTIFICATION_PROCESS_LIST` (comma/semicolon-separated owner names) | Override the default whitelist; falls back to the built-in 7 apps when unset |
| — | `AETHERFLOW_NOTIFICATION_MASK_POLL_FRAMES` (default 5) | Window-list poll cadence in frames |
| `--privacy-mask-mode=<blackout\|blur\|mosaic>` | `AETHERFLOW_PRIVACY_MASK_MODE` | Compositor visual mode. Defaults: Windows `blur`; macOS `mosaic` |
| `--panic-mask` / `--privacy-mask-fullscreen` | `AETHERFLOW_PRIVACY_PANIC_MASK` (default `0`) | Start with a full-screen panic mask. Windows only |
| `--panic-mask-hotkey` / `--no-panic-mask-hotkey` | `AETHERFLOW_PRIVACY_PANIC_HOTKEY` (default `1`) | Enable / disable the Right Ctrl dev/test panic latch. Windows only |
| `--privacy-mask=<left,top,right,bottom>` | `AETHERFLOW_PRIVACY_MASKS` (semicolon-separated rectangles) | Add one or more manual mask rectangles. Windows only |
| — | `AETHERFLOW_PRIVACY_MASK_MOSAIC_BLOCK_PX` | Mosaic cell size in pixels |
| `--mock-analyzer` / `--no-mock-analyzer` | `AETHERFLOW_MOCK_ANALYZER` (default `0`) | Wire `MockSlowAnalyzer` (real-thread mock: dedicated `std::thread` worker, mutex + condvar, configurable inference delay) through `AsyncAnalyzerBridgeModule` (P2 + Bridge Hardening 2026-05-12). Mock publishes a `mock-slow-analyzer` scene at confidence 0.92 from its worker thread, with `inferenceMs` measured per submission. Verifies the async bridge + confidence-based merge (P1) without pulling in a real model. Cross-platform (Windows + macOS). |
| `--analyzer-bridge-interval-frames=N` | `AETHERFLOW_ANALYZER_BRIDGE_INTERVAL_FRAMES` (default `1`) | Sub-sampling cadence for `AsyncAnalyzerBridgeModule::SubmitFrame`. `N=1` submits every frame (default for the mock analyzer); `N=5` submits 1 frame in 5, the rest reuse the last cached analysis. Lets a real ~200 ms-per-inference analyzer keep up with a 30 fps producer without flooding the queue. **When the scene classifier is the active analyzer and no explicit override is given, the interval defaults to `AETHERFLOW_FPS` (≈1 Hz readback at 30 fps), not `1`** — this keeps the producer-thread D3D11 staging-texture readback off the fast path; an explicit CLI/env value always wins. Cross-platform (Windows + macOS). |
| — | `AETHERFLOW_MOCK_ANALYZER_INFERENCE_MS` (default `200`) | Per-inference wall-clock delay the `MockSlowAnalyzer` worker thread sleeps for before publishing a result. Tunes mock fidelity for bridge stress tests. Cross-platform (Windows + macOS). |
| `--scene-classifier-onnx-model=<path>` | `AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL` (default unset) | Phase 4 P0.1 Windows scene classifier. When set, loads the CLIP zero-shot ONNX model (`models/scene_classifier_v1.onnx`) and wires `SceneClassifierOnnx` through `AsyncAnalyzerBridgeModule` (replaces the mock when both are set). Activates the cross-platform `PolicyEngine` (advisory) and emits strategy-A trace fields `sceneClass` / `sceneClassConfidence` / `policyMode` / `policyReason`. Default off — when unset the binary is byte-equivalent to the pre-P0.1 build. Windows only. |
| `--scene-classifier-provider=<directml\|cpu>` | `AETHERFLOW_SCENE_CLASSIFIER_PROVIDER` (default `directml`) | ONNX Runtime execution provider preference. `directml` (default) tries the DirectML EP and falls back to CPU EP if DirectML construction fails; `cpu` forces the CPU EP. The actually-loaded provider is printed in the `[Runtime] AsyncAnalyzerBridgeModule active` startup banner. Windows only. |
| `--scene-classifier-demo-action` / `--no-scene-classifier-demo-action` | `AETHERFLOW_SCENE_CLASSIFIER_DEMO_ACTION` (default `0`) | **Opt-in visual demo (Phase 4 §4.y), default OFF — NOT P1.1, NOT product behavior.** Only does anything when the scene classifier is also active (`--scene-classifier-onnx-model=...`); a no-op warning is printed otherwise. When ON, registers `SceneDemoActionModule` after `PolicyEngine` so it sees the final merged scene, and applies a full-screen privacy-mask effect chosen per detected class: `sensitive_surface`→blackout, `video`→mosaic, `code_text`→blur, `slides`→grayscale, `mixed_ui`/unknown→passthrough (no mask). Suppressed on any frame where a deterministic producer (panic/password/notification/manual) already emitted a mask — real masks always win. Self-describes via existing schema-v3 trace fields (`privacyMaskSource="scene-demo-action"`, `privacyMaskPath` = the chosen shader); no schema bump. OFF ⇒ byte-equivalent to a classifier-only run. Windows only. |

### ONNX Runtime setup (Phase 4 P0.1 scene classifier)

The scene classifier links the vendored ONNX Runtime + DirectML build under
`third_party/onnxruntime/`. The directory is gitignored; recreate it before
building with the SOP in
[`third_party/onnxruntime/SOURCE.md`](../third_party/onnxruntime/SOURCE.md).
Pinned package: `Microsoft.ML.OnnxRuntime.DirectML` **1.20.1**. Quick
PowerShell download:

```powershell
# Run from the repository root.
$repo = (Get-Location).Path
$thirdParty = Join-Path $repo "third_party"
$ort = Join-Path $thirdParty "onnxruntime"
$tmp = Join-Path $thirdParty "_ort_dml.zip"
$extract = Join-Path $thirdParty "_ort_dml_extract"
New-Item -ItemType Directory -Force -Path `
  (Join-Path $ort "include"), (Join-Path $ort "lib"), (Join-Path $ort "bin") | Out-Null
Invoke-WebRequest -Uri "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1" -OutFile $tmp
Expand-Archive -Force -Path $tmp -DestinationPath $extract
Copy-Item (Join-Path $extract "build\native\include\*.h") (Join-Path $ort "include")
Copy-Item (Join-Path $extract "runtimes\win-x64\native\onnxruntime.lib") (Join-Path $ort "lib")
Copy-Item (Join-Path $extract "runtimes\win-x64\native\onnxruntime.dll") (Join-Path $ort "bin")
Remove-Item -Recurse -Force -LiteralPath $extract
Remove-Item -Force -LiteralPath $tmp
```

If the headers are absent, CMake auto-disables the classifier
(`AETHERFLOW_ENABLE_SCENE_CLASSIFIER` → OFF) with a warning and the rest of
the binary still builds — the classifier source is `#ifdef`-guarded.
`DirectML.dll` is sourced from `C:\Windows\System32` (ships with Windows
10/11). Validate the wiring with:

```powershell
python tools\agent_run.py --run-id scene_classifier_onnx_smoke -- --scene-classifier-onnx-model=models/scene_classifier_v1.onnx
python tools\agent_verify.py --run-dir .aetherflow\runs\scene_classifier_onnx_smoke
```

This Windows scene-classifier run writes under
`.aetherflow/runs/scene_classifier_onnx_smoke/` (verified 2026-05-15):

- `artifacts/output/output_encoded.h264` (NVENC H.264 bitstream — written
  by default; disable with `AETHERFLOW_NVENC_WRITE_BITSTREAM=0`)
- `frame_trace.jsonl` (one JSON line per frame; when the classifier is
  active it adds the strategy-A fields `sceneClass` /
  `sceneClassConfidence` / `policyMode` / `policyReason`)
- `trace.summary.json` (`platform="windows"`, `roi_supported=true`,
  schema_version 3; classifier aggregates `scene_classifier_active`,
  `scene_inference_count`, `scene_inference_p95_ms`,
  `scene_inference_failures`, `scene_class_distribution`,
  `policy_mode_switches`, `policy_fallback_frames`,
  `policy_low_confidence_frames` — all present, zeroed when the
  classifier is inactive)
- `verify_report.json` (build / smoke / trace gates plus the additive
  `scene_classifier` gate block — 4 gates, `passed` when the classifier
  is active, `not_applicable` when inactive; the `analyzer_bridge` block
  is unchanged)

Correct behavior: a classifier-active run reports
`verify_report.json status: passed` with `scene_classifier.status: passed`
(all 4 sub-gates) and a non-degenerate `scene_class_distribution` (more
than one class — proof that real captured pixels reach the model, not a
constant). A classifier-inactive run (no `--scene-classifier-onnx-model`)
reports `scene_classifier.status: not_applicable` and is byte-equivalent
on the non-classifier trace fields to the pre-P0.1 baseline. A degenerate
single-class distribution on a varied-content screen would mean the
real-pixel readback path is not wired — not a model accuracy problem.
Real-screen classification *accuracy* is intentionally **not** gated here — the
gate only proves real pixels reach the model and the output varies. Measuring
accuracy is a separate manual review against ground truth, not a build gate.

### macOS run artifacts and phase scope

Each macOS run writes its artifacts under `.aetherflow/runs/<run_id>/`:

- `artifacts/output/output.mp4` (AVAssetWriter MP4 container)
- `frame_trace.jsonl` (one JSON line per frame; Windows-aligned schema)
- `trace.summary.json` (includes `platform="macos"`, `roi_supported=false`,
  and the macOS compositor trace paths `coreimage-bgra-clearfill`,
  `coreimage-bgra-blur`, `coreimage-bgra-mosaic`, and the fail-closed
  `coreimage-bgra-clearfill-fallback`)
- `macos_smoke.json` (capture/encode counters, duration, output path, permission state)
- `verify_report.json` (build / smoke / trace gates; phase-2 added four mask
  gates: applied == total when total > 0, no fallback frames, no
  `clearfill-fallback` path, and passthrough = `{"none": frames}`-only)

Phase scope:

- Phase 1: capture + encode + MP4 mux + trace (verified 2026-05-10).
- Phase 2: chat-window mosaic producer + CoreImage/Metal compositor (verified
  2026-05-11). Sub-phase still open: AXUIElement secure-text-field producer.
  (No Mac panic-hotkey is planned — the Windows Right Ctrl trigger is a
  dev/test affordance, not a product feature.)
- Phase 3: ROI/QP exploration (currently `roi_supported=false` on Mac).

Honest constraint: the ROI benchmark (`tools/roi_benchmark.ps1`) is
Windows-only (NVENC / VPL); `--run-benchmark` on macOS is a no-op warning while
`roi_supported=false`.

Pixel-format note: macOS capture is now BGRA (flipped from phase-1 NV12) so
the CoreImage compositor can filter a single-plane buffer. VideoToolbox
accepts BGRA directly and converts to NV12 internally; the output H.264
bitstream is unchanged. On the phase-2 canonical run this was a net latency
win at both capture (-1.7 %) and encodeSubmit (-96 %) versus phase 1.

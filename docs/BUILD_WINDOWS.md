# Build AetherFlow from a Fresh Windows Clone

This is the canonical Windows source-build procedure for both people and
coding agents. The PowerShell bootstrap is the executable source of truth; do
not replace its pinned dependency steps with guessed downloads.

## What the default build includes

The default `Studio` profile builds the product shown in the screenshots:

- `AetherFlow.exe`, the command-line capture and encode pipeline;
- `AetherFlowStudio.exe`, the settings-window version of the same pipeline;
- deterministic privacy masks for Windows UI Automation password fields and
  recognized application windows such as LINE and Teams;
- SRT live streaming of the already-masked encoded output;
- the first-party CTest suite and the Studio UI smoke test.

The privacy product does **not** depend on the ONNX scene classifier. The
classifier is an optional, advisory whole-screen scene guesser; it does not
drive privacy masks, encoding, or SRT in the default product path. The
bootstrap therefore does not download ONNX Runtime or a model and explicitly
builds with the classifier disabled.

## Prerequisites

Install these before cloning:

1. Windows 10 1903 or newer, or Windows 11.
2. Visual Studio 2022 or Build Tools 2022 with the **Desktop development with
   C++** workload, including MSVC v143 and a Windows 10 or 11 SDK.
3. CMake 3.20 or newer, available as `cmake` and `ctest` on `PATH`.
4. Python 3, available as `python` on `PATH`.
5. Git and a network connection for the pinned ImGui and FFmpeg downloads.

At runtime, an encoder backend also needs supported hardware and a current
driver:

- Intel Gen 6 (Skylake) or newer can use the vendored oneVPL build.
- NVIDIA Maxwell or newer can use NVENC after the NVIDIA SDK header is supplied
  as described below.
- There is no CPU software encoder fallback.

## Recommended fresh-clone build

Run these commands in PowerShell:

```powershell
git clone https://github.com/darker901-chen/Aetherflow.git
Set-Location Aetherflow
Set-ExecutionPolicy -Scope Process Bypass -Force
.\tools\bootstrap_windows.ps1
```

The bootstrap performs the following deterministic sequence:

1. checks CMake, CTest, Python 3, and the requested Visual Studio generator;
2. downloads the repository-pinned Dear ImGui and LGPL FFmpeg/SRT packages and
   verifies their SHA-256 hashes;
3. configures an x64 Visual Studio build with ONNX scene classification off;
4. builds the CLI, Studio UI, and first-party tests;
5. runs CTest and `AetherFlowStudio.exe --ui-smoke`;
6. prints the exact executable paths and an enabled/disabled capability
   summary.

Success means the command exits with code 0 and prints `Build completed
successfully.` The expected executables are:

```text
build\Release\AetherFlow.exe
build\Release\AetherFlowStudio.exe
```

Start the UI with:

```powershell
.\build\Release\AetherFlowStudio.exe
```

## Build profiles and options

### Core-only build

Use this when only the headless pipeline is needed. It does not fetch ImGui or
FFmpeg and explicitly disables SRT and the ONNX classifier.

```powershell
.\tools\bootstrap_windows.ps1 -Profile Core
```

### Reuse dependencies already fetched

```powershell
.\tools\bootstrap_windows.ps1 -SkipDependencyFetch
```

The Studio profile fails clearly if the required ImGui or FFmpeg files are not
already present. It never silently produces a reduced Studio build.

### Visual Studio 2019 or a different build directory

```powershell
.\tools\bootstrap_windows.ps1 -VisualStudioVersion 2019 -BuildDir build/vs2019
```

Do not reuse one build directory with a different CMake generator. Select a
new directory under the gitignored `build/` tree if the existing directory was
configured by another generator.

### NVIDIA NVENC

The NVIDIA Video Codec SDK header cannot be redistributed in this repository.
After accepting NVIDIA's terms and downloading the SDK, either copy
`Interface\nvEncodeAPI.h` to the repository location documented in
[`external/VideoCodecSDK/SOURCE.md`](../external/VideoCodecSDK/SOURCE.md), or
pass the SDK root directly:

```powershell
.\tools\bootstrap_windows.ps1 -NvencSdkRoot 'C:\SDKs\Video_Codec_SDK_13.0.19'
```

Without the header, the build still succeeds and reports NVENC as disabled.
An NVIDIA-only computer then has no usable encoder until the header is added
and the project is reconfigured.

### Optional ONNX scene classifier

ONNX Runtime and `models\scene_classifier_v1.onnx` are intentionally excluded
from the bootstrap because of their size and distribution cost. They are not
required for password-field or application-window privacy masks.

If classifier experimentation is needed, follow
[`third_party/onnxruntime/SOURCE.md`](../third_party/onnxruntime/SOURCE.md) and
the ONNX section of the [Operation Guide](OPERATION_GUIDE.md). Then reconfigure
manually with `-DAETHERFLOW_ENABLE_SCENE_CLASSIFIER=ON`. Treat the result as an
advisory or demo feature, not as a verified privacy detector.

## Capability boundaries

| Capability | Default Studio bootstrap | Additional requirement |
|---|---:|---|
| Windows capture | Built | Windows Graphics Capture support |
| Password-field masking | Built and on by default | Visible UIA password field |
| LINE/Teams/etc. window masking | Built and on by default | Recognized visible application window |
| Blur/blackout/mosaic compositor | Built | None |
| Studio UI | Built | Pinned ImGui fetched by the bootstrap |
| SRT masked stream | Built, opt-in at runtime | Pinned FFmpeg fetched; viewer and firewall access |
| Intel oneVPL encoder | Built | Supported Intel GPU and driver |
| NVIDIA NVENC encoder | Conditional | NVIDIA SDK header plus supported GPU and driver |
| ONNX scene classification | Disabled | Manual ONNX Runtime and model setup |

`Build completed successfully` proves source configuration, compilation, unit
tests, and the UI startup smoke. It does not prove that the current computer
has a usable encoder, that another device can cross its firewall, or that an
optional AI model is accurate.

## Coding-agent execution contract

A coding agent asked to build AetherFlow on Windows should:

1. read this file and run `tools\bootstrap_windows.ps1` from the repository
   root in PowerShell;
2. use the default Studio profile unless the user explicitly requests Core;
3. never download ONNX Runtime, a model, or NVIDIA SDK material on its own;
4. treat a nonzero bootstrap exit as a failed build and report the first failed
   named step;
5. report the final capability summary exactly, including disabled NVENC or AI;
6. never claim runtime capture/encode/SRT success from build and UI-smoke
   evidence alone.

For product operation, SRT viewer setup, flags, runtime verification, and
packaging, continue with the [Operation Guide](OPERATION_GUIDE.md).

## Common failures

| Symptom | Action |
|---|---|
| Visual Studio generator not found | Add the **Desktop development with C++** workload, or select the installed VS version. |
| Existing build directory uses another generator | Rerun with a new path under `build/`, such as `-BuildDir build/vs2022-fresh`. |
| `python` not found | Install Python 3 and enable its `PATH` option. |
| Hash verification fails | Do not bypass it; remove the failed gitignored download and retry from a trusted network. |
| Studio dependency missing with `-SkipDependencyFetch` | Rerun without that switch. |
| Build succeeds but no encoder starts | Check the GPU/driver boundary above; NVIDIA-only systems also need the SDK header. |
| SRT works locally but not from another device | Allow the selected UDP port through Windows Firewall and use the printed LAN URL. |

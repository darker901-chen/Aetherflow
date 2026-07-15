# ONNX Runtime (third_party/onnxruntime)

Vendored from the official Microsoft.ML.OnnxRuntime.DirectML NuGet package.
This directory is gitignored; bytes are not checked in. Recreate it locally
with the steps below before running a build.

## Pinned version

- **NuGet package:** `Microsoft.ML.OnnxRuntime.DirectML`
- **Version:** `1.20.1`
- **Source URL:** `https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1`

## Layout produced by the SOP below

```
third_party/onnxruntime/
  include/                   # C and C++ headers (onnxruntime_cxx_api.h, dml_provider_factory.h, ...)
  lib/onnxruntime.lib        # win-x64 import library (linked by AetherFlow)
  bin/onnxruntime.dll        # win-x64 runtime DLL (copied next to AetherFlow.exe by POST_BUILD)
```

DirectML.dll is sourced from Windows itself (`C:\Windows\System32\DirectML.dll`).
On a development workstation it is present out of the box on Windows 10/11;
no separate redistributable is required for the development gates G1-G5. A
follow-on hardening pass (P1+) can vendor `Microsoft.AI.DirectML` for a fully
self-contained ship.

## Download SOP (PowerShell)

```powershell
# Run from the repository root.
$repo = (Get-Location).Path
$thirdParty = Join-Path $repo "third_party"
$ort = Join-Path $thirdParty "onnxruntime"
$tmp = Join-Path $thirdParty "_ort_dml.zip"
$extract = Join-Path $thirdParty "_ort_dml_extract"
New-Item -ItemType Directory -Force -Path `
  (Join-Path $ort "include"), (Join-Path $ort "lib"), (Join-Path $ort "bin") | Out-Null
Invoke-WebRequest `
  -Uri "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1" `
  -OutFile $tmp
Expand-Archive -Force -Path $tmp -DestinationPath $extract
Copy-Item (Join-Path $extract "build\native\include\*.h") (Join-Path $ort "include")
Copy-Item (Join-Path $extract "runtimes\win-x64\native\onnxruntime.lib") (Join-Path $ort "lib")
Copy-Item (Join-Path $extract "runtimes\win-x64\native\onnxruntime.dll") (Join-Path $ort "bin")
Remove-Item -Recurse -Force -LiteralPath $extract
Remove-Item -Force -LiteralPath $tmp
```

## Download SOP (bash / curl)

```bash
# Run from the repository root.
REPO="$(pwd)"
ORT="$REPO/third_party/onnxruntime"
TMP="$REPO/third_party/_ort_dml.nupkg"
EXTRACT="$REPO/third_party/_ort_dml_extract"
mkdir -p "$ORT/include" "$ORT/lib" "$ORT/bin"
curl -L -o "$TMP" "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1"
unzip -o -q "$TMP" -d "$EXTRACT"
cp "$EXTRACT"/build/native/include/*.h "$ORT/include/"
cp "$EXTRACT/runtimes/win-x64/native/onnxruntime.lib" "$ORT/lib/"
cp "$EXTRACT/runtimes/win-x64/native/onnxruntime.dll" "$ORT/bin/"
rm -rf -- "$EXTRACT"
rm -f -- "$TMP"
```

## When the build skips ONNX Runtime

`CMakeLists.txt` defines `AETHERFLOW_ENABLE_SCENE_CLASSIFIER` (default ON on
Windows). When `third_party/onnxruntime/include/onnxruntime_cxx_api.h` is
missing the option auto-disables with a warning, and the runtime falls back
to the same binary AetherFlow shipped before P0.1 — the `SceneClassifierOnnx`
source file is `#ifdef`-guarded so the build still succeeds without ORT.

## Why this path

- Matches the existing `third_party/onevpl/` pattern (vendored, gitignored,
  documented). No new top-level package manager (vcpkg) was introduced.
- Pip-installed `onnxruntime` (CPU-only, 1.26.0) does not expose the
  DirectML provider this project targets, so re-using it would force CPU EP
  permanently.
- The Microsoft.ML.OnnxRuntime.DirectML NuGet ships one DLL with both the
  CPU EP and the DirectML EP, so a single download covers both code paths.

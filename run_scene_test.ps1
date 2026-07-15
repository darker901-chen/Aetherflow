# run_scene_test.ps1 — one-button scene-classifier eyeball test.
#
# Build (unless -SkipBuild) -> run AetherFlow.exe with the ONNX scene
# classifier enabled -> print, in plain language, what the AI guessed,
# who won the scene merge, and what actually got masked on screen.
#
# Usage:
#   .\run_scene_test.ps1                       # default 900 frames
#   .\run_scene_test.ps1 -Frames 1800          # ~60s of content
#   .\run_scene_test.ps1 -SkipBuild            # skip cmake build
#   .\run_scene_test.ps1 -OutDir D:\mytest     # choose output folder
param(
    [string]$OutDir   = (Join-Path $PSScriptRoot "scene_test_out"),
    [string]$Model    = "models/scene_classifier_v1.onnx",
    [int]   $Frames   = 0,                                  # 0 = exe default (900)
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $PSScriptRoot "build\Release\AetherFlow.exe"

if (-not $SkipBuild) {
    Write-Host "[1/3] building (cmake --build build --config Release)..." -ForegroundColor Cyan
    cmake --build (Join-Path $PSScriptRoot "build") --config Release --target AetherFlow
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
} else {
    Write-Host "[1/3] skip build" -ForegroundColor DarkGray
}

if (-not (Test-Path $exe)) { throw "AetherFlow.exe not found at $exe (build first)" }
if (-not (Test-Path (Join-Path $PSScriptRoot $Model))) {
    throw "model not found: $Model — run 'python tools/export_clip_zeroshot.py' first"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$env:AETHERFLOW_OUTPUT_DIR = $OutDir
if ($Frames -gt 0) { $env:AETHERFLOW_MAX_FRAMES = "$Frames" }

Write-Host "[2/3] running AetherFlow.exe (classifier ON)... output -> $OutDir" -ForegroundColor Cyan
Write-Host "      arrange your screen NOW (code editor / video / slides / LINE window / etc.)" -ForegroundColor Yellow
& $exe "--scene-classifier-onnx-model=$Model"
if ($LASTEXITCODE -ne 0) { Write-Host "exe exit $LASTEXITCODE (continuing to report)" -ForegroundColor DarkYellow }

$trace = Join-Path $OutDir "traces\frame_trace.jsonl"
if (-not (Test-Path $trace)) { throw "no trace at $trace" }

Write-Host "[3/3] AI scene report" -ForegroundColor Cyan
$py = @'
import json, sys, collections
path = sys.argv[1]
ai = collections.Counter(); won = collections.Counter(); masked = collections.Counter()
confs = []; n = 0
for line in open(path, encoding="utf-8"):
    line = line.strip()
    if not line: continue
    d = json.loads(line); n += 1
    sc = d.get("sceneClass")
    if sc:
        ai[sc] += 1
        c = d.get("sceneClassConfidence")
        if isinstance(c, (int, float)): confs.append(c)
    won[d.get("sceneSource", "-")] += 1
    masked[d.get("privacyMaskSource", "none")] += 1
print(f"  total frames        : {n}")
print(f"  AI guessed (sceneClass)        : {dict(ai) or '(classifier never contributed — flag missing?)'}")
if confs:
    confs.sort()
    p = lambda q: confs[min(len(confs)-1, int(len(confs)*q))]
    print(f"  AI confidence min/med/max      : {confs[0]:.3f} / {p(0.5):.3f} / {confs[-1]:.3f}")
print(f"  who WON the scene (sceneSource): {dict(won)}")
print(f"  what got MASKED on screen      : {dict(masked)}")
ai_won = won.get("scene-classifier-onnx", 0)
det = won.get("notification-producer", 0) + won.get("panic-privacy-mask", 0) + won.get("password-field-privacy-mask", 0)
print()
print(f"  -> AI's vote actually took effect on {ai_won} frames")
print(f"  -> a deterministic mask producer overrode the AI on {det} frames (expected when LINE/Teams/password/panic present)")
print(f"  -> reminder: P0.1 is observe-only; the AI vote does NOT change the video. Only deterministic masks do.")
'@
$py | python - $trace
Write-Host ""
Write-Host "video + raw trace are under: $OutDir" -ForegroundColor DarkGray

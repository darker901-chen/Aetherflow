# AetherFlow Live Share Guard demo runner
#
# One-shot launcher. With the v0.2 default flip every deterministic detector
# (Right Ctrl panic hotkey, password-field UIA, notification process whitelist)
# is on by default and the visual mode is blur, so this script mostly:
#   1. cd to the repo root
#   2. set AETHERFLOW_OUTPUT_DIR so the bitstream lands in <repo>/output/
#   3. run AetherFlow.exe with no extra flags
#   4. mux the NVENC raw .h264 into a playable demo.mp4 if ffmpeg is on PATH
#   5. open Explorer at the output directory
#
# Override examples:
#   $env:AETHERFLOW_PRIVACY_MASK_MODE = 'mosaic';   .\demo.ps1
#   $env:AETHERFLOW_NOTIFICATION_MASK = '0';         .\demo.ps1
#   .\demo.ps1 -- --panic-mask                       # passes args to the exe

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot

$exe = Join-Path $repoRoot 'build\Release\AetherFlow.exe'
if (-not (Test-Path $exe)) {
    Write-Host "[demo] AetherFlow.exe not found at $exe" -ForegroundColor Yellow
    Write-Host "[demo] Building Release first..."
    cmake --build build --config Release --target AetherFlow
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[demo] Build failed (exit $LASTEXITCODE). Aborting." -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

$outputDir = Join-Path $repoRoot 'output'
$env:AETHERFLOW_OUTPUT_DIR = $outputDir
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

# Pass-through args: anything after `--` from the caller is forwarded to the
# exe. Without args the binary uses the v0.2 defaults (blur + every detector).
$forwardArgs = @()
$dashDashSeen = $false
foreach ($a in $args) {
    if (-not $dashDashSeen -and $a -eq '--') {
        $dashDashSeen = $true
        continue
    }
    $forwardArgs += $a
}

Write-Host '[demo] Live Share Guard defaults: blur + panic hotkey + password-field + notification (LINE/Slack/Discord/Teams/Telegram/WhatsApp)' -ForegroundColor Cyan
Write-Host "[demo] Running: $exe $($forwardArgs -join ' ')" -ForegroundColor Cyan
& $exe @forwardArgs

$h264 = Join-Path $outputDir 'output_encoded.h264'
$mp4  = Join-Path $outputDir 'demo.mp4'

if (Test-Path $h264) {
    Write-Host "[demo] Bitstream: $h264" -ForegroundColor Green
    $ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue)
    if ($ffmpeg) {
        if (Test-Path $mp4) { Remove-Item $mp4 -Force }
        & $ffmpeg.Source -hide_banner -loglevel error -r 30 -i $h264 -c copy $mp4
        if ($LASTEXITCODE -eq 0 -and (Test-Path $mp4)) {
            Write-Host "[demo] Muxed to mp4: $mp4" -ForegroundColor Green
        } else {
            Write-Host "[demo] ffmpeg mux failed (exit $LASTEXITCODE); raw .h264 still available" -ForegroundColor Yellow
        }
    } else {
        Write-Host "[demo] ffmpeg not on PATH; skipped mp4 mux. Play raw with ffplay or run:" -ForegroundColor Yellow
        Write-Host "       ffmpeg -r 30 -i `"$h264`" -c copy `"$mp4`"" -ForegroundColor Yellow
    }
} else {
    Write-Host "[demo] No bitstream produced. Check console output above." -ForegroundColor Red
}

Start-Process explorer.exe $outputDir

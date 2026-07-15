<#
.SYNOPSIS
    AetherFlow ROI 效果量化基準測試腳本

.DESCRIPTION
    自動執行三輪螢幕錄製，計算三項指標來量化 ROI 效果：

      PSNR              : 像素失真度（dB，越高越好）
      SSIM              : 結構相似度 0~1（越接近 1 越好）
                          比 PSNR 更接近人眼感知，特別是文字/UI 邊緣
      Frame Size StdDev : 幀大小標準差（bytes，越低 = 碼率越穩定 = 網路 jitter 越小）
                          ROI 把 bits 引導到滑鼠區域後分配更精準，幀大小波動應下降

    三輪錄製流程：
      [1/3] Reference  16000 kbps, ROI off  -> 高碼率基準（PSNR/SSIM 的參考）
      [2/3] ROI ON     1500 kbps,  DeltaQP=-30  -> 滑鼠區域拿到更多 bits
      [3/3] ROI OFF    1500 kbps,  DeltaQP=0    -> 均勻分配（對照組）

.PARAMETER MouseX
    ROI 中心 X 座標（像素）。
    追蹤模式：錄製時把滑鼠移到這個位置並保持不動。
    靜態模式（-Static）：直接寫入 Config.h，不需要管滑鼠位置。

.PARAMETER MouseY
    ROI 中心 Y 座標（像素）。

.PARAMETER Static
    加上此 switch 後，ROI 固定在 (MouseX, MouseY)，不追蹤滑鼠位置。
    推薦：benchmark 時加 -Static，結果可重現，不受錄製期間滑鼠飄移影響。

.PARAMETER RoiRadius
    ROI 半徑（像素），對應 Config.h AETHERFLOW_ROI_RADIUS。

.PARAMETER AnalyzeFrame
    從錄製結果中取第幾幀做截圖比較（預設第 60 幀）。

.EXAMPLE
    # 固定座標模式（推薦，結果可重現）
    .\roi_benchmark.ps1 -MouseX 1080 -MouseY 620 -Static

    # 滑鼠追蹤模式（錄製時讓滑鼠停在目標位置不動）
    .\roi_benchmark.ps1 -MouseX 1080 -MouseY 620
#>

param(
    [string]$RepoRoot    = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$OutputDir   = "",
    [int]$AnalyzeFrame   = 60,
    [int]$MouseX         = 1280,
    [int]$MouseY         = 720,
    [int]$RoiRadius      = 200,
    [switch]$Static                # 固定 ROI 座標，不追蹤滑鼠（推薦用於 benchmark）
)

$ErrorActionPreference = "Stop"
if (-not $OutputDir) { $OutputDir = "$RepoRoot\output" }

$configFile = "$RepoRoot\include\AetherFlow\Config.h"
$buildDir   = "$RepoRoot\build"
$exePath    = "$buildDir\Release\AetherFlow.exe"
$tmpDir     = "$OutputDir\_benchmark_tmp"
$reportFile = "$OutputDir\roi_report.md"
$refFile    = "$OutputDir\reference.mp4"
$roiOnFile  = "$OutputDir\roi_on.mp4"
$roiOffFile = "$OutputDir\roi_off.mp4"

New-Item -ItemType Directory -Path $tmpDir    -Force | Out-Null
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

# check ffmpeg
try { & ffmpeg -version 2>&1 | Out-Null }
catch { Write-Error "ffmpeg not found. Run: winget install Gyan.FFmpeg"; exit 1 }

if (-not (Test-Path $buildDir)) {
    Write-Error "build\ not found. Run cmake first."
    exit 1
}

# read resolution from Config.h
$cfg    = Get-Content $configFile -Raw
$width  = if ($cfg -match '#define AETHERFLOW_WIDTH\s+(\d+)')  { [int]$Matches[1] } else { 1920 }
$height = if ($cfg -match '#define AETHERFLOW_HEIGHT\s+(\d+)') { [int]$Matches[1] } else { 1080 }
$fps    = if ($cfg -match '#define AETHERFLOW_FPS\s+(\d+)')    { [int]$Matches[1] } else { 30 }

# backup Config.h
$configBackup = "$tmpDir\Config.h.bak"
Copy-Item $configFile $configBackup -Force

function Restore-Config {
    Copy-Item $configBackup $configFile -Force
    Write-Host "Config.h restored."
}
trap { Write-Host "[ERROR] $_"; Restore-Config; exit 1 }

# Set-Define: replace "#define NAME value" line-by-line, preserve trailing comment
function Set-Define([string]$defineName, [string]$newValue) {
    $lines = [System.IO.File]::ReadAllLines($configFile)
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]
        if ($line -match "^\s*#define\s+$defineName\s+") {
            $comment = ""
            $slashIdx = $line.IndexOf("//")
            if ($slashIdx -ge 0) { $comment = "  " + $line.Substring($slashIdx) }
            $lines[$i] = "#define $defineName $newValue$comment"
        }
    }
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllLines($configFile, $lines, $utf8NoBom)
}

function Build-Project([string]$label) {
    Write-Host "  [Build] $label ..."
    $out = & cmake --build $buildDir --config Release --target AetherFlow 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host $out; throw "Build failed: $label" }
    Write-Host "  [Build] done"
}

function Run-Capture([string]$label, [string[]]$exeArgs = @()) {
    $modeDesc = if ($exeArgs.Count) { "static ROI @ ($MouseX,$MouseY)" } else { "keep mouse at $MouseX,$MouseY" }
    Write-Host "  [Run] $label  ($modeDesc) ..."
    if (-not (Test-Path $exePath)) { throw "exe not found: $exePath" }
    Remove-Item "$OutputDir\output.mp4" -Force -ErrorAction SilentlyContinue
    Remove-Item "$OutputDir\output_encoded.h264" -Force -ErrorAction SilentlyContinue
    $oldOutputDir = $env:AETHERFLOW_OUTPUT_DIR
    $oldNvencWrite = $env:AETHERFLOW_NVENC_WRITE_BITSTREAM
    $env:AETHERFLOW_OUTPUT_DIR = $OutputDir
    $env:AETHERFLOW_NVENC_WRITE_BITSTREAM = "1"
    try {
        & $exePath @exeArgs 2>&1 | Select-Object -Last 5 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { throw "AetherFlow.exe failed" }
    } finally {
        $env:AETHERFLOW_OUTPUT_DIR = $oldOutputDir
        $env:AETHERFLOW_NVENC_WRITE_BITSTREAM = $oldNvencWrite
    }
    Start-Sleep -Milliseconds 500
}

function Ensure-Mp4Output([string]$label) {
    $mp4 = "$OutputDir\output.mp4"
    $h264 = "$OutputDir\output_encoded.h264"
    if (Test-Path $mp4) {
        return
    }
    if (Test-Path $h264) {
        Write-Host "  [Run] $label produced H.264 bitstream; remuxing to output.mp4 ..."
        & ffmpeg -hide_banner -loglevel error -framerate $fps -i $h264 -c:v copy $mp4 -y
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $mp4)) {
            throw "Failed to remux output_encoded.h264 to output.mp4"
        }
        return
    }
    throw "No encoder output found: expected output.mp4 or output_encoded.h264"
}

# ---- Mode banner (no Config.h changes needed for static ROI -- passed via CLI args to exe) ----
if ($Static) {
    Write-Host "  [Mode] Static ROI fixed at ($MouseX, $MouseY) -- pass --roi-x/--roi-y to exe, no need to move mouse"
} else {
    Write-Host "  [Mode] Mouse tracking -- keep mouse at ($MouseX, $MouseY) during all captures"
}

# ---- Step 1: reference (high bitrate, ROI off) ----
Write-Host "`n[1/3] reference  16000kbps  ROI off"
Set-Define "AETHERFLOW_BITRATE"      "16000"
Set-Define "AETHERFLOW_ROI_DELTA_QP" "0"
Build-Project "reference"
Run-Capture   "reference" $(if ($Static) { @("--roi-x", $MouseX, "--roi-y", $MouseY) } else { @() })
Ensure-Mp4Output "reference"
Copy-Item "$OutputDir\output.mp4" $refFile -Force
Write-Host "  -> saved reference.mp4"

# ---- Step 2: ROI ON ----
Write-Host "`n[2/3] ROI ON  1500kbps  DeltaQP=-30"
Set-Define "AETHERFLOW_BITRATE"      "1500"
Set-Define "AETHERFLOW_ROI_DELTA_QP" "(-30)"
Build-Project "roi_on"
Run-Capture   "roi_on"  $(if ($Static) { @("--roi-x", $MouseX, "--roi-y", $MouseY) } else { @() })
Ensure-Mp4Output "roi_on"
Copy-Item "$OutputDir\output.mp4" $roiOnFile -Force
Write-Host "  -> saved roi_on.mp4"

# ---- Step 3: ROI OFF ----
Write-Host "`n[3/3] ROI OFF  1500kbps  DeltaQP=0"
Set-Define "AETHERFLOW_BITRATE"      "1500"
Set-Define "AETHERFLOW_ROI_DELTA_QP" "0"
Build-Project "roi_off"
Run-Capture   "roi_off" $(if ($Static) { @("--roi-x", $MouseX, "--roi-y", $MouseY) } else { @() })
Ensure-Mp4Output "roi_off"
Copy-Item "$OutputDir\output.mp4" $roiOffFile -Force
Write-Host "  -> saved roi_off.mp4"

Restore-Config

# ---- analysis ----
$roiX = [Math]::Max(0, $MouseX - $RoiRadius)
$roiY = [Math]::Max(0, $MouseY - $RoiRadius)
$roiW = [Math]::Min($RoiRadius * 2, $width  - $roiX)
$roiH = [Math]::Min($RoiRadius * 2, $height - $roiY)
$bgW  = 400; $bgH = 300
$bgX  = $width - $bgW; $bgY = $height - $bgH

Write-Host "`n[analyze 1/3] extract frame $AnalyzeFrame ..."
foreach ($pair in @(
    @{ src=$refFile;    dst="frame_ref.png"     }
    @{ src=$roiOnFile;  dst="frame_roi_on.png"  }
    @{ src=$roiOffFile; dst="frame_roi_off.png" }
)) {
    & ffmpeg -hide_banner -loglevel error -i $pair.src `
        -vf "select=eq(n\,$AnalyzeFrame)" -vframes 1 "$tmpDir\$($pair.dst)" -y
}

Write-Host "[analyze 2/3] crop screenshots ..."
foreach ($c in @(
    @{ src="frame_ref.png";     dst="crop_ref_roi.png";  x=$roiX; y=$roiY; w=$roiW; h=$roiH }
    @{ src="frame_roi_on.png";  dst="crop_on_roi.png";   x=$roiX; y=$roiY; w=$roiW; h=$roiH }
    @{ src="frame_roi_off.png"; dst="crop_off_roi.png";  x=$roiX; y=$roiY; w=$roiW; h=$roiH }
    @{ src="frame_ref.png";     dst="crop_ref_bg.png";   x=$bgX;  y=$bgY;  w=$bgW;  h=$bgH  }
    @{ src="frame_roi_on.png";  dst="crop_on_bg.png";    x=$bgX;  y=$bgY;  w=$bgW;  h=$bgH  }
    @{ src="frame_roi_off.png"; dst="crop_off_bg.png";   x=$bgX;  y=$bgY;  w=$bgW;  h=$bgH  }
)) {
    & ffmpeg -hide_banner -loglevel error `
        -i "$tmpDir\$($c.src)" `
        -vf "crop=$($c.w):$($c.h):$($c.x):$($c.y)" `
        "$tmpDir\$($c.dst)" -y
}

# PSNR/SSIM/FrameSize: parse from ffmpeg stderr -- avoids stats_file Windows path escaping entirely.
function Get-PSNR([string]$refMp4, [string]$testMp4, [int]$cx, [int]$cy, [int]$cw, [int]$ch) {
    $filter = "[0:v]crop=${cw}:${ch}:${cx}:${cy}[va]" +
              ";[1:v]crop=${cw}:${ch}:${cx}:${cy}[vb]" +
              ";[va][vb]psnr"
    # Capture all output (summary line is AV_LOG_INFO so -loglevel error would suppress it).
    # We parse internally and only echo lines that look like real warnings/errors.
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $out = & ffmpeg -hide_banner -i $refMp4 -i $testMp4 -lavfi $filter -f null - 2>&1
    } finally {
        $ErrorActionPreference = $oldEap
    }
    $out = $out | ForEach-Object { [string]$_ }
    $out | Where-Object { $_ -match '(Error|Warning|Invalid|failed)' } | ForEach-Object { Write-Host "    [ffmpeg] $_" }
    $m = $out | Select-String 'average:([0-9.]+)' | Select-Object -Last 1
    if (-not $m) { return "N/A" }
    return [math]::Round([float]$m.Matches[0].Groups[1].Value, 2)
}

# SSIM: 結構相似度 0~1，比 PSNR 更接近人眼感知（文字/UI 邊緣保留程度）
function Get-SSIM([string]$refMp4, [string]$testMp4, [int]$cx, [int]$cy, [int]$cw, [int]$ch) {
    $filter = "[0:v]crop=${cw}:${ch}:${cx}:${cy}[va]" +
              ";[1:v]crop=${cw}:${ch}:${cx}:${cy}[vb]" +
              ";[va][vb]ssim"
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $out = & ffmpeg -hide_banner -i $refMp4 -i $testMp4 -lavfi $filter -f null - 2>&1
    } finally {
        $ErrorActionPreference = $oldEap
    }
    $out = $out | ForEach-Object { [string]$_ }
    $out | Where-Object { $_ -match '(Error|Warning|Invalid|failed)' } | ForEach-Object { Write-Host "    [ffmpeg] $_" }
    $m = $out | Select-String 'All:([0-9.]+)' | Select-Object -Last 1
    if (-not $m) { return "N/A" }
    return [math]::Round([float]$m.Matches[0].Groups[1].Value, 4)
}

# Frame Size StdDev: 幀大小標準差（bytes）
# CBR 下 ROI 把 bits 引導到滑鼠區域後分配更精準 -> 幀大小波動降低 -> 網路 jitter 改善
function Get-FrameSizeStdDev([string]$mp4) {
    $out = & ffprobe -hide_banner -loglevel error -select_streams v:0 `
        -show_packets -show_entries "packet=size" -of csv=p=0 $mp4 2>&1
    $sizes = $out | Where-Object { $_ -match '^\d+$' } | ForEach-Object { [double]$_ }
    if (-not $sizes -or $sizes.Count -lt 2) { return "N/A" }
    $mean     = ($sizes | Measure-Object -Average).Average
    $variance = ($sizes | ForEach-Object { ($_ - $mean) * ($_ - $mean) } | Measure-Object -Average).Average
    return [math]::Round([math]::Sqrt($variance))
}

Write-Host "[analyze 3/4] computing PSNR (~30s) ..."
$onRoi  = Get-PSNR $refFile $roiOnFile  $roiX $roiY $roiW $roiH
$offRoi = Get-PSNR $refFile $roiOffFile $roiX $roiY $roiW $roiH
$onBg   = Get-PSNR $refFile $roiOnFile  $bgX  $bgY  $bgW  $bgH
$offBg  = Get-PSNR $refFile $roiOffFile $bgX  $bgY  $bgW  $bgH

Write-Host "[analyze 4/4] computing SSIM + frame size variance (~30s) ..."
$ssimOnRoi  = Get-SSIM $refFile $roiOnFile  $roiX $roiY $roiW $roiH
$ssimOffRoi = Get-SSIM $refFile $roiOffFile $roiX $roiY $roiW $roiH
$ssimOnBg   = Get-SSIM $refFile $roiOnFile  $bgX  $bgY  $bgW  $bgH
$ssimOffBg  = Get-SSIM $refFile $roiOffFile $bgX  $bgY  $bgW  $bgH

$fsStdOn  = Get-FrameSizeStdDev $roiOnFile
$fsStdOff = Get-FrameSizeStdDev $roiOffFile

function Fmt-Diff([string]$a, [string]$b, [bool]$higherIsBetter) {
    if ($a -eq "N/A" -or $b -eq "N/A") { return "N/A" }
    $d = [math]::Round([float]$a - [float]$b, 2)
    $ok = if ($higherIsBetter) { $d -ge 0 } else { $d -le 0 }
    $sign = if ($d -ge 0) { "+" } else { "" }
    $mark = if ($ok) { "OK" } else { "!!" }
    return "$sign$d dB [$mark]"
}

$diffRoi = Fmt-Diff $onRoi  $offRoi $true   # ROI area: higher ON = better
$diffBg  = Fmt-Diff $onBg   $offBg  $false  # BG area:  lower ON = better

# SSIM diff (no unit label -- it's a 0~1 ratio, not dB)
$ssimDiffRoi = if ($ssimOnRoi -ne "N/A" -and $ssimOffRoi -ne "N/A") {
    $d = [math]::Round([float]$ssimOnRoi - [float]$ssimOffRoi, 4)
    $sign = if ($d -ge 0) { "+" } else { "" }
    $mark = if ($d -ge 0) { "OK" } else { "!!" }
    "$sign${d} [$mark]"
} else { "N/A" }
$ssimDiffBg = if ($ssimOnBg -ne "N/A" -and $ssimOffBg -ne "N/A") {
    $d = [math]::Round([float]$ssimOnBg - [float]$ssimOffBg, 4)
    $sign = if ($d -ge 0) { "+" } else { "" }
    $mark = if ($d -le 0) { "OK" } else { "!!" }
    "$sign${d} [$mark]"
} else { "N/A" }

# Frame size variance verdict
$fsNote = if ($fsStdOn -ne "N/A" -and $fsStdOff -ne "N/A") {
    if ([double]$fsStdOn -le [double]$fsStdOff) { "roi_on stddev <= roi_off  ==> ROI stabilized bitrate [OK]" }
    else { "roi_on stddev > roi_off  ==> ROI increased variance [!!]" }
} else { "Frame size data unavailable." }

$sRef    = [math]::Round((Get-Item $refFile).Length    / 1KB)
$sOn     = [math]::Round((Get-Item $roiOnFile).Length  / 1KB)
$sOff    = [math]::Round((Get-Item $roiOffFile).Length / 1KB)

# ---- write report (ASCII-safe line by line) ----
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("# AetherFlow ROI Benchmark Report")
$lines.Add("")
$lines.Add("Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm')")
$lines.Add("")
$lines.Add("| Parameter | Value |")
$lines.Add("|-----------|-------|")
$lines.Add("| Resolution | ${width}x${height} |")
$lines.Add("| Test bitrate | 1500 kbps |")
$lines.Add("| ROI radius | ${RoiRadius} px |")
$lines.Add("| ROI DeltaQP | -30 |")
$lines.Add("| Mouse position | (${MouseX}, ${MouseY}) |")
$lines.Add("| Analyzed frame | $AnalyzeFrame |")
$lines.Add("")
$lines.Add("---")
$lines.Add("")
$lines.Add("## File Size")
$lines.Add("")
$lines.Add("| File | Size | Description |")
$lines.Add("|------|------|-------------|")
$lines.Add("| reference.mp4 | $sRef KB | 16000 kbps high-quality reference |")
$lines.Add("| roi_on.mp4    | $sOn KB  | 1500 kbps, ROI DeltaQP=-30 |")
$lines.Add("| roi_off.mp4   | $sOff KB | 1500 kbps, ROI DeltaQP=0 (uniform) |")
$lines.Add("")
$lines.Add("> roi_on ~= roi_off in size = bits redistributed, not wasted.")
$lines.Add("")
$lines.Add("---")
$lines.Add("")
$lines.Add("## PSNR vs High-Bitrate Reference")
$lines.Add("")
$lines.Add("> Higher PSNR = closer to reference = more bits allocated to that region.")
$lines.Add("")
$lines.Add("| Region | ROI ON | ROI OFF | ON minus OFF |")
$lines.Add("|--------|--------|---------|--------------|")
$lines.Add("| ROI center (mouse +${RoiRadius}px) | **${onRoi} dB** | ${offRoi} dB | ${diffRoi} |")
$lines.Add("| Background (bottom-right ${bgW}x${bgH}) | ${onBg} dB | **${offBg} dB** | ${diffBg} |")
$lines.Add("")
$lines.Add("### Reading the result")
$lines.Add("- ROI center ON - OFF **positive** = ROI redirected bits to mouse area [OK]")
$lines.Add("- Background ON - OFF **negative** = background gave up bits [OK]")
$lines.Add("")
$lines.Add("---")
$lines.Add("")
$lines.Add("## SSIM vs High-Bitrate Reference")
$lines.Add("")
$lines.Add("> SSIM 0~1, higher = more structurally similar to reference.")
$lines.Add("> More perceptually accurate than PSNR -- captures edge/texture similarity (important for text & UI).")
$lines.Add("")
$lines.Add("| Region | ROI ON | ROI OFF | ON minus OFF |")
$lines.Add("|--------|--------|---------|--------------|")
$lines.Add("| ROI center (mouse +${RoiRadius}px) | **${ssimOnRoi}** | ${ssimOffRoi} | ${ssimDiffRoi} |")
$lines.Add("| Background (bottom-right ${bgW}x${bgH}) | ${ssimOnBg} | **${ssimOffBg}** | ${ssimDiffBg} |")
$lines.Add("")
$lines.Add("---")
$lines.Add("")
$lines.Add("## Frame Size Variance (Network Jitter)")
$lines.Add("")
$lines.Add("> Per-frame encoded size std-dev (bytes). Lower = more stable bitrate = less network jitter.")
$lines.Add("> In CBR mode, ROI redistributes bits more precisely -> frame size should be more consistent for roi_on.")
$lines.Add("")
$lines.Add("| File | Frame Size StdDev (bytes) | Mode |")
$lines.Add("|------|--------------------------|------|")
$lines.Add("| roi_on.mp4  | ${fsStdOn}  | 1500 kbps + ROI DeltaQP=-30 |")
$lines.Add("| roi_off.mp4 | ${fsStdOff} | 1500 kbps uniform (no ROI)  |")
$lines.Add("")
$lines.Add("> $fsNote")
$lines.Add("")
$lines.Add("---")
$lines.Add("")
$lines.Add("## Screenshots")
$lines.Add("")
$lines.Add("Folder: $tmpDir")
$lines.Add("")
$lines.Add("| | ROI ON | ROI OFF | Reference (high bitrate) |")
$lines.Add("|--|--------|---------|--------------------------|")
$lines.Add("| ROI center | crop_on_roi.png | crop_off_roi.png | crop_ref_roi.png |")
$lines.Add("| Background | crop_on_bg.png  | crop_off_bg.png  | crop_ref_bg.png  |")

$utf8NoBom = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllLines($reportFile, $lines.ToArray(), $utf8NoBom)

# ---- summary ----
Write-Host ""
Write-Host "========================================"
Write-Host "  ROI Benchmark Result"
Write-Host "========================================"
Write-Host ("  [PSNR]   ROI center : ON={0,6} dB   OFF={1,6} dB   diff={2}" -f $onRoi,      $offRoi,     $diffRoi)
Write-Host ("  [PSNR]   Background : ON={0,6} dB   OFF={1,6} dB   diff={2}" -f $onBg,       $offBg,      $diffBg)
Write-Host ("  [SSIM]   ROI center : ON={0,6}      OFF={1,6}      diff={2}" -f $ssimOnRoi,  $ssimOffRoi, $ssimDiffRoi)
Write-Host ("  [SSIM]   Background : ON={0,6}      OFF={1,6}      diff={2}" -f $ssimOnBg,   $ssimOffBg,  $ssimDiffBg)
Write-Host ("  [Jitter] FrameSize StdDev : ROI_ON={0} B   ROI_OFF={1} B" -f $fsStdOn, $fsStdOff)
Write-Host "  [Jitter] $fsNote"
Write-Host "========================================"
Write-Host ""
Write-Host "Report : $reportFile"
Write-Host "Shots  : $tmpDir"
Write-Host ""
Write-Host "Done."

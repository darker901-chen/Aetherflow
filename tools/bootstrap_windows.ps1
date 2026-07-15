<#
.SYNOPSIS
Configures, builds, and verifies AetherFlow from a fresh Windows clone.

.DESCRIPTION
The default Studio profile fetches the repository-pinned Dear ImGui and
FFmpeg packages, configures a Visual Studio x64 build, builds the CLI, Studio,
and tests, runs CTest, and runs the Studio UI smoke test.

The ONNX scene classifier is deliberately disabled. It is optional/advisory
and is not required by the deterministic UI Automation and application-window
privacy masks.

.PARAMETER Profile
Studio (default) builds the showcased UI and SRT support. Core builds the
headless executable without fetching optional UI or streaming dependencies.

.PARAMETER BuildDir
Build directory, relative to the repository root unless an absolute path is
provided. The default is build.

.PARAMETER VisualStudioVersion
Visual Studio generator version: 2022 (default) or 2019.

.PARAMETER Configuration
CMake configuration to build. The default is Release.

.PARAMETER NvencSdkRoot
Optional NVIDIA Video Codec SDK root containing Interface\nvEncodeAPI.h.
The SDK header is not redistributed by this repository.

.PARAMETER SkipDependencyFetch
Do not run the pinned ImGui and FFmpeg fetchers. Studio mode then requires
those dependencies to already be present in third_party.

.PARAMETER SkipTests
Build only; skip CTest and the Studio UI smoke test.

.EXAMPLE
.\tools\bootstrap_windows.ps1

.EXAMPLE
.\tools\bootstrap_windows.ps1 -Profile Core

.EXAMPLE
.\tools\bootstrap_windows.ps1 -NvencSdkRoot 'C:\SDKs\Video_Codec_SDK_13.0.19'
#>
[CmdletBinding()]
param(
    [ValidateSet('Core', 'Studio')]
    [string]$Profile = 'Studio',

    [string]$BuildDir = 'build',

    [ValidateSet('2022', '2019')]
    [string]$VisualStudioVersion = '2022',

    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [string]$NvencSdkRoot = '',

    [switch]$SkipDependencyFetch,

    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$ArgumentList,

        [Parameter(Mandatory = $true)]
        [string]$Step
    )

    Write-Host "`n==> $Step" -ForegroundColor Cyan
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$Step failed with exit code $LASTEXITCODE."
    }
}

function Require-Command {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$InstallHint
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "Required command '$Name' was not found. $InstallHint"
    }
    return $command.Source
}

if ($PSVersionTable.PSEdition -eq 'Core' -and -not $IsWindows) {
    throw 'This bootstrap supports Windows only. See docs/OPERATION_GUIDE.md for macOS.'
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    [System.IO.Path]::GetFullPath($BuildDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $BuildDir))
}

$CMake = Require-Command -Name 'cmake' -InstallHint 'Install CMake 3.20 or newer and add it to PATH.'
$Ctest = Require-Command -Name 'ctest' -InstallHint 'CTest is installed with CMake; add the CMake bin directory to PATH.'

$cmakeVersionLine = (& $CMake --version | Select-Object -First 1)
if ($cmakeVersionLine -notmatch '(\d+\.\d+\.\d+)') {
    throw "Could not parse the CMake version from: $cmakeVersionLine"
}
$cmakeVersion = [version]$Matches[1]
if ($cmakeVersion -lt [version]'3.20.0') {
    throw "CMake 3.20 or newer is required; found $cmakeVersion."
}

$Python = $null
if ($Profile -eq 'Studio' -and -not $SkipDependencyFetch) {
    $Python = Require-Command -Name 'python' -InstallHint 'Install Python 3 and add python.exe to PATH.'
    $pythonVersionLine = (& $Python --version 2>&1 | Select-Object -First 1).ToString()
    if ($pythonVersionLine -notmatch '^Python 3\.') {
        throw "Python 3 is required; found: $pythonVersionLine"
    }
}

Write-Host 'AetherFlow Windows bootstrap' -ForegroundColor Green
Write-Host "  Repository:     $RepoRoot"
Write-Host "  Profile:        $Profile"
Write-Host "  Build directory: $BuildPath"
Write-Host "  Configuration:  $Configuration"
Write-Host "  CMake:          $cmakeVersion"

Push-Location $RepoRoot
try {
    if ($Profile -eq 'Studio' -and -not $SkipDependencyFetch) {
        Invoke-NativeCommand -FilePath $Python -ArgumentList @('tools/fetch_imgui.py') -Step 'Fetch pinned Dear ImGui sources'
        Invoke-NativeCommand -FilePath $Python -ArgumentList @('tools/fetch_ffmpeg.py') -Step 'Fetch pinned LGPL FFmpeg/SRT SDK'
    }

    $imguiHeader = Join-Path $RepoRoot 'third_party/imgui/imgui.h'
    $ffmpegHeader = Join-Path $RepoRoot 'third_party/ffmpeg/include/libavformat/avformat.h'
    if ($Profile -eq 'Studio') {
        if (-not (Test-Path -LiteralPath $imguiHeader -PathType Leaf)) {
            throw 'Studio profile requires Dear ImGui. Rerun without -SkipDependencyFetch or run python tools/fetch_imgui.py.'
        }
        if (-not (Test-Path -LiteralPath $ffmpegHeader -PathType Leaf)) {
            throw 'Studio profile requires the FFmpeg SDK for SRT. Rerun without -SkipDependencyFetch or run python tools/fetch_ffmpeg.py.'
        }
    }

    $generator = if ($VisualStudioVersion -eq '2022') {
        'Visual Studio 17 2022'
    } else {
        'Visual Studio 16 2019'
    }

    $configureArgs = @(
        '-S', $RepoRoot,
        '-B', $BuildPath,
        '-G', $generator,
        '-A', 'x64',
        '-DAETHERFLOW_BUILD_TESTS=ON',
        '-DAETHERFLOW_ENABLE_SCENE_CLASSIFIER=OFF'
    )
    if ($Profile -eq 'Studio') {
        $configureArgs += '-DAETHERFLOW_ENABLE_SRT_OUTPUT=ON'
    } else {
        $configureArgs += '-DAETHERFLOW_ENABLE_SRT_OUTPUT=OFF'
    }

    $nvencHeader = $null
    if ($NvencSdkRoot) {
        $NvencSdkRoot = [System.IO.Path]::GetFullPath($NvencSdkRoot)
        $nvencHeader = Join-Path $NvencSdkRoot 'Interface/nvEncodeAPI.h'
        if (-not (Test-Path -LiteralPath $nvencHeader -PathType Leaf)) {
            throw "NvencSdkRoot does not contain Interface\nvEncodeAPI.h: $NvencSdkRoot"
        }
        $configureArgs += "-DNVENC_SDK_ROOT=$NvencSdkRoot"
    } else {
        $configureArgs += '-DNVENC_SDK_ROOT='
        $repoNvencHeader = Join-Path $RepoRoot 'external/VideoCodecSDK/Interface/nvEncodeAPI.h'
        if (Test-Path -LiteralPath $repoNvencHeader -PathType Leaf) {
            $nvencHeader = $repoNvencHeader
        }
    }
    $configureArgs += '-DAETHERFLOW_DISABLE_NVENC=OFF'

    Invoke-NativeCommand -FilePath $CMake -ArgumentList $configureArgs -Step 'Configure the Visual Studio build'

    $buildTargets = @('AetherFlow')
    if ($Profile -eq 'Studio') {
        $buildTargets += 'AetherFlowStudio'
    }
    if (-not $SkipTests) {
        $buildTargets += @(
            'aetherflow_test_frame_decision',
            'aetherflow_test_policy_engine',
            'aetherflow_test_annexb',
            'aetherflow_test_appconfig'
        )
    }
    $buildArgs = @('--build', $BuildPath, '--config', $Configuration, '--parallel', '--target') + $buildTargets
    Invoke-NativeCommand -FilePath $CMake -ArgumentList $buildArgs -Step "Build targets: $($buildTargets -join ', ')"

    $binaryDir = Join-Path $BuildPath $Configuration
    $coreExe = Join-Path $binaryDir 'AetherFlow.exe'
    $studioExe = Join-Path $binaryDir 'AetherFlowStudio.exe'
    if (-not (Test-Path -LiteralPath $coreExe -PathType Leaf)) {
        throw "Expected core executable was not generated: $coreExe"
    }
    if ($Profile -eq 'Studio' -and -not (Test-Path -LiteralPath $studioExe -PathType Leaf)) {
        throw "Expected Studio executable was not generated: $studioExe"
    }

    if (-not $SkipTests) {
        Invoke-NativeCommand -FilePath $Ctest -ArgumentList @('--test-dir', $BuildPath, '--build-config', $Configuration, '--output-on-failure') -Step 'Run first-party CTest suite'

        if ($Profile -eq 'Studio') {
            Write-Host "`n==> Run Studio UI smoke test" -ForegroundColor Cyan
            $smoke = Start-Process -FilePath $studioExe -ArgumentList '--ui-smoke' -WorkingDirectory $binaryDir -Wait -PassThru
            if ($smoke.ExitCode -ne 0) {
                throw "Studio UI smoke test failed with exit code $($smoke.ExitCode)."
            }
        }
    }

    Write-Host "`nBuild completed successfully." -ForegroundColor Green
    Write-Host "  Core CLI:       $coreExe"
    if ($Profile -eq 'Studio') {
        Write-Host "  Studio UI:      $studioExe"
        Write-Host '  SRT streaming:  enabled (pinned FFmpeg SDK present)'
    } else {
        Write-Host '  Studio UI:      not requested by the Core profile'
        Write-Host '  SRT streaming:  disabled by the Core profile'
    }
    Write-Host '  Privacy masks:  enabled (UI Automation password fields + application-window detection)'
    Write-Host '  AI classifier:  disabled (optional/advisory; not required for privacy masking)'
    if ($null -ne $nvencHeader) {
        Write-Host "  NVENC source:   enabled from $nvencHeader"
    } else {
        Write-Host '  NVENC source:   disabled (SDK header not present; oneVPL remains built)'
    }
    if ($SkipTests) {
        Write-Host '  Verification:   skipped by request'
    } else {
        Write-Host '  Verification:   CTest passed; Studio UI smoke passed when applicable'
    }
    Write-Host '  Runtime note:   encoder availability still depends on a supported GPU and current driver.'
}
finally {
    Pop-Location
}

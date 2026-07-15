param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$BuildDir = "",
    [string]$Config = "Release",
    [string]$RunDir = "",
    [switch]$RunBenchmark
)

$ErrorActionPreference = "Stop"

$python = if ($env:PYTHON) {
    $env:PYTHON
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    "python"
} else {
    "py"
}

$script = Join-Path $PSScriptRoot "agent_verify.py"
$argsList = @($script, "--repo-root", $RepoRoot, "--config", $Config)
if ($BuildDir) { $argsList += @("--build-dir", $BuildDir) }
if ($RunDir) { $argsList += @("--run-dir", $RunDir) }
if ($RunBenchmark) { $argsList += "--run-benchmark" }

& $python @argsList
exit $LASTEXITCODE

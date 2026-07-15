param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$BuildDir = "",
    [string]$Config = "Release",
    [string]$RunId = "",
    [string[]]$ProgramArgs = @(),
    [switch]$SkipBuild,
    [switch]$SkipRun
)

$ErrorActionPreference = "Stop"

$python = if ($env:PYTHON) {
    $env:PYTHON
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    "python"
} else {
    "py"
}

$script = Join-Path $PSScriptRoot "agent_run.py"
$argsList = @($script, "--repo-root", $RepoRoot, "--config", $Config)
if ($BuildDir) { $argsList += @("--build-dir", $BuildDir) }
if ($RunId) { $argsList += @("--run-id", $RunId) }
if ($SkipBuild) { $argsList += "--skip-build" }
if ($SkipRun) { $argsList += "--skip-run" }
if ($ProgramArgs.Count -gt 0) { $argsList += @("--") + $ProgramArgs }

& $python @argsList
exit $LASTEXITCODE

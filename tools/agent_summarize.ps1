param(
    [Parameter(Mandatory=$true)]
    [string]$RunDir,
    [int]$MaxEvidenceLines = 80
)

$ErrorActionPreference = "Stop"

$python = if ($env:PYTHON) {
    $env:PYTHON
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    "python"
} else {
    "py"
}

$script = Join-Path $PSScriptRoot "agent_summarize.py"
& $python $script --run-dir $RunDir --max-evidence-lines $MaxEvidenceLines
exit $LASTEXITCODE

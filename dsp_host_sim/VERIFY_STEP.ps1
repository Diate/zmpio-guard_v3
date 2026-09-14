param(
    [string]$PythonPath = ""
)

$ErrorActionPreference = "Stop"
$stepRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

& (Join-Path $stepRoot "BUILD_STEP.ps1") -Configuration Release
if ($LASTEXITCODE -ne 0) { throw "Release verification failed" }
& (Join-Path $stepRoot "BUILD_STEP.ps1") -Configuration Debug
if ($LASTEXITCODE -ne 0) { throw "Debug verification failed" }
& (Join-Path $stepRoot "VERIFY_UPSTREAM.ps1")
if ($LASTEXITCODE -ne 0) { throw "Upstream verification failed" }

if (-not $PythonPath) {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCommand) {
        $PythonPath = $pythonCommand.Source
    }
}
if (-not $PythonPath) {
    # py.exe (the Windows launcher) resolves an interpreter that is installed
    # but not on PATH.
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    if ($launcher) {
        $resolved = & $launcher.Source -c "import sys; print(sys.executable)" 2>$null
        if ($LASTEXITCODE -eq 0 -and $resolved) { $PythonPath = $resolved.Trim() }
    }
}
if (-not $PythonPath -or -not (Test-Path -LiteralPath $PythonPath -PathType Leaf)) {
    throw "Python interpreter not found; pass -PythonPath explicitly"
}

& $PythonPath (Join-Path $stepRoot "tools\test_python_tools.py")
if ($LASTEXITCODE -ne 0) { throw "Python tool smoke tests failed" }

Write-Host "Step1 full local verification PASS"

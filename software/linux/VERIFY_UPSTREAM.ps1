$ErrorActionPreference = "Stop"
$stepRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent (Split-Path -Parent $stepRoot)
$manifestPath = Join-Path $stepRoot "config\upstream_manifest.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$failed = $false

# Same normalization as dsp_host_sim/VERIFY_UPSTREAM.ps1 -- see that file's
# comment for why raw Get-FileHash is not environment-independent.
function Get-NormalizedSha256 {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $normalized = New-Object System.Collections.Generic.List[byte]
    foreach ($b in $bytes) {
        if ($b -ne 0x0D) { $normalized.Add($b) }
    }
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $hashBytes = $sha256.ComputeHash($normalized.ToArray())
    return -join ($hashBytes | ForEach-Object { $_.ToString("X2") })
}

foreach ($entry in $manifest.baseline_files) {
    $path = Join-Path $projectRoot $entry.path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Write-Error "Missing upstream file: $($entry.path)" -ErrorAction Continue
        $failed = $true
        continue
    }
    $actual = Get-NormalizedSha256 -Path $path
    if ($actual -ne $entry.sha256) {
        Write-Error "Hash mismatch: $($entry.path)" -ErrorAction Continue
        $failed = $true
    } else {
        Write-Host "OK  $($entry.path)"
    }
}

if ($failed) {
    throw "Step 6 upstream verification failed"
}
Write-Host "Step 6 upstream verification PASS"

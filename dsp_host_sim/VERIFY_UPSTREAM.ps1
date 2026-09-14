$ErrorActionPreference = "Stop"
$stepRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $stepRoot
$manifestPath = Join-Path $stepRoot "config\upstream_manifest.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$failed = $false

# Hash is computed over content with CRLF normalized to LF (i.e. the same
# bytes git stores in the blob), NOT the raw checked-out file. Without this,
# the manifest is only reproducible on whatever core.autocrlf setting/OS
# generated it -- on a Windows checkout with core.autocrlf=true every text
# file mismatches by line ending alone, which is indistinguishable from a
# real upstream content change (see docs/PLAN_BUOC_6.md R4).
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
    throw "Step1 upstream verification failed"
}
Write-Host "Step1 upstream verification PASS"


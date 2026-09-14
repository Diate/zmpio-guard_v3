<#
.SYNOPSIS
    Confirm by SHA-256 that the bitstream / XSA / ps7_init.tcl / ELF copies in the
    Vitis workspace really are the latest ones exported from Vivado.

.DESCRIPTION
    The Xilinx toolchain keeps several independent copies of the same artefact:
    the original XSA and bitstream in the Vivado project, the copy that
    update_hw() imports into platform_dual\hw\, and a per-application launch
    cache under <app>\_ide\bitstream\ and <app>\_ide\psinit\. Pressing Build or
    Refresh Platform in the Vitis GUI does not guarantee that all of those layers
    move together.

    Timestamps lie: a copy preserves mtime, and two different files can share a
    build date. SHA-256 is the only reliable check, so this script never concludes
    "fresh" from a timestamp alone.

.PARAMETER VivadoProjectDir
    Vivado project directory, containing *.xpr and *.runs\impl_1\.
    Defaults to hardware\vivado\project_hub in this repository.

.PARAMETER WorkspaceDir
    Vitis workspace directory, containing platform_dual\, app_freertos\ and
    cpu0_application\. Defaults to firmware\ in this repository.

.EXAMPLE
    powershell -File scripts\verify_hw_freshness.ps1

.EXAMPLE
    powershell -File scripts\verify_hw_freshness.ps1 -VivadoProjectDir <alternate-project-dir>
#>

param(
    [string]$VivadoProjectDir,
    [string]$WorkspaceDir
)

$ErrorActionPreference = "Stop"

Import-Module (Join-Path $PSScriptRoot 'ProjectPaths.psm1') -Force
$zmpio = Get-ZmpioPaths
if (-not $VivadoProjectDir) { $VivadoProjectDir = $zmpio.VivadoProjectDir }
if (-not $WorkspaceDir)     { $WorkspaceDir     = $zmpio.FirmwareDir }

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Get-Sha256FromZipEntry([string]$ZipPath, [string]$EntryNameLower) {
    if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf)) { return $null }
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
    $zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName.ToLowerInvariant().EndsWith($EntryNameLower) } | Select-Object -First 1
        if (-not $entry) { return $null }
        $stream = $entry.Open()
        try {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            $hashBytes = $sha.ComputeHash($stream)
            return ([BitConverter]::ToString($hashBytes) -replace '-', '')
        } finally {
            $stream.Dispose()
        }
    } finally {
        $zip.Dispose()
    }
}

function Write-Row([string]$Label, [string]$Path, [string]$ExpectedHash, [string]$ActualHash) {
    $exists = Test-Path -LiteralPath $Path -PathType Leaf
    if (-not $exists) {
        Write-Host ("  [MISSING] {0,-32} {1}" -f $Label, $Path) -ForegroundColor DarkGray
        return
    }
    $mtime = (Get-Item -LiteralPath $Path).LastWriteTime
    if ([string]::IsNullOrEmpty($ActualHash)) { $ActualHash = Get-Sha256 $Path }
    if ($ActualHash -eq $ExpectedHash) {
        Write-Host ("  [OK]      {0,-32} {1}  ({2:yyyy-MM-dd HH:mm})" -f $Label, $Path, $mtime) -ForegroundColor Green
    } else {
        Write-Host ("  [STALE]   {0,-32} {1}  ({2:yyyy-MM-dd HH:mm})" -f $Label, $Path, $mtime) -ForegroundColor Red
        Write-Host ("            expected {0}" -f $ExpectedHash) -ForegroundColor Red
        Write-Host ("            actual   {0}" -f $ActualHash) -ForegroundColor Red
    }
}

Write-Host "=== 1) Source of truth: newest bitstream and XSA from Vivado ===" -ForegroundColor Cyan

$sourceBit = Get-ChildItem -Path $VivadoProjectDir -Recurse -File -Filter "*.bit" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\impl_1\\' } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$sourceXsa = Get-ChildItem -Path $VivadoProjectDir -Filter "*.xsa" -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1

if (-not $sourceBit) { Write-Error "No .bit found under *.runs\impl_1 in $VivadoProjectDir" }
if (-not $sourceXsa) { Write-Error "No .xsa found in $VivadoProjectDir" }

$sourceBitHash = Get-Sha256 $sourceBit.FullName
$sourceXsaFileHash = Get-Sha256 $sourceXsa.FullName
$sourceXsaEmbeddedBitHash = Get-Sha256FromZipEntry $sourceXsa.FullName ".bit"

Write-Host ("  Source bitstream : {0}  ({1:yyyy-MM-dd HH:mm})" -f $sourceBit.FullName, $sourceBit.LastWriteTime)
Write-Host ("    SHA-256        : {0}" -f $sourceBitHash)
Write-Host ("  Source XSA       : {0}  ({1:yyyy-MM-dd HH:mm})" -f $sourceXsa.FullName, $sourceXsa.LastWriteTime)

if ($sourceXsaEmbeddedBitHash -eq $sourceBitHash) {
    Write-Host "  [OK] The bitstream embedded in the XSA matches the standalone bitstream" -ForegroundColor Green
} else {
    Write-Host "  [WARNING] XSA and bitstream are NOT a matched pair - check that both come from the same run" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== 2) Bitstream copies cached in the Vitis workspace ===" -ForegroundColor Cyan
Write-Row "platform_dual\hw (imported XSA)" (Join-Path $WorkspaceDir "platform_dual\hw\$($sourceXsa.BaseName).xsa") $sourceXsaFileHash $null
Write-Row "app_freertos _ide bitstream" (Join-Path $WorkspaceDir "app_freertos\_ide\bitstream\cpu1_wrapper.bit") $sourceBitHash $null
Write-Row "cpu0_application _ide bitstream" (Join-Path $WorkspaceDir "cpu0_application\_ide\bitstream\cpu_wrapper.bit") $sourceBitHash $null

Write-Host ""
Write-Host "=== 3) ps7_init.tcl against the copy embedded in the source XSA ===" -ForegroundColor Cyan
$sourcePs7InitHash = Get-Sha256FromZipEntry $sourceXsa.FullName "ps7_init.tcl"
if ($sourcePs7InitHash) {
    Write-Row "platform_dual\hw\sdt\ps7_init.tcl" (Join-Path $WorkspaceDir "platform_dual\hw\sdt\ps7_init.tcl") $sourcePs7InitHash $null
    Write-Row "cpu0_application _ide psinit" (Join-Path $WorkspaceDir "cpu0_application\_ide\psinit\ps7_init.tcl") $sourcePs7InitHash $null
    Write-Row "app_freertos _ide psinit (if present)" (Join-Path $WorkspaceDir "app_freertos\_ide\psinit\ps7_init.tcl") $sourcePs7InitHash $null
} else {
    Write-Host "  [SKIPPED] No ps7_init.tcl embedded in the source XSA" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "=== 4) Output ELF - newer than the newest .c/.h source? ===" -ForegroundColor Cyan
$elfChecks = @(
    @{ Elf = Join-Path $zmpio.OutputDir "cpu1\app_freertos.elf";       Src = Join-Path $WorkspaceDir "app_freertos\src" }
    @{ Elf = Join-Path $zmpio.OutputDir "cpu0\cpu0_application.elf";   Src = Join-Path $WorkspaceDir "cpu0_application\src" }
)
foreach ($check in $elfChecks) {
    if (-not (Test-Path -LiteralPath $check.Elf -PathType Leaf)) {
        Write-Host ("  [MISSING] {0}" -f $check.Elf) -ForegroundColor DarkGray
        continue
    }
    $elfTime = (Get-Item -LiteralPath $check.Elf).LastWriteTime
    $newestSrc = Get-ChildItem -Path $check.Src -Recurse -File -Include *.c,*.h -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newestSrc -and $newestSrc.LastWriteTime -gt $elfTime) {
        Write-Host ("  [STALE]   {0}  (elf {1:yyyy-MM-dd HH:mm} is OLDER than source {2} {3:yyyy-MM-dd HH:mm})" -f $check.Elf, $elfTime, $newestSrc.Name, $newestSrc.LastWriteTime) -ForegroundColor Red
    } else {
        Write-Host ("  [OK]      {0}  (elf {1:yyyy-MM-dd HH:mm})" -f $check.Elf, $elfTime) -ForegroundColor Green
    }
    $sha256File = "$($check.Elf).sha256"
    if (Test-Path -LiteralPath $sha256File) {
        $recorded = (Get-Content -LiteralPath $sha256File -Raw).Trim().Split(' ')[0]
        $actual = Get-Sha256 $check.Elf
        if ($recorded -eq $actual) {
            Write-Host "  [OK]      .sha256 sidecar matches the current file contents" -ForegroundColor Green
        } else {
            Write-Host "  [STALE]   .sha256 sidecar does NOT match - the file was overwritten after the sidecar was written" -ForegroundColor Red
        }
    }
}

Write-Host ""
Write-Host "=== Legend ===" -ForegroundColor Cyan
Write-Host "  [OK]      hash matches the newest Vivado output - safe to program or build on."
Write-Host "  [STALE]   contents differ from the source even if the timestamp looks recent - rebuild or re-sync first."
Write-Host "  [MISSING] the file does not exist at that location yet."

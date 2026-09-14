param(
    # Defaults to ARM_LINUX_CC from config/toolchain.env.
    [string]$CrossCompiler
)

Import-Module (Join-Path $PSScriptRoot '../../scripts/ProjectPaths.psm1') -Force
if (-not $CrossCompiler) { $CrossCompiler = Get-ZmpioTool -Name ARM_LINUX_CC }

$ErrorActionPreference = "Stop"
$stepRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $stepRoot "build"

if (-not (Test-Path -LiteralPath $CrossCompiler)) {
    throw "ARM Linux cross compiler not found: $CrossCompiler (override with -CrossCompiler, or install PetaLinux's own toolchain and point at arm-linux-gnueabihf-gcc)"
}

$cmake = Get-Command cmake -ErrorAction Stop

# Cross-compiling for ARM Linux, not Windows -- the default Visual Studio
# generator ignores CMAKE_C_COMPILER for its per-config toolset (it silently
# picks MSVC and fails on every POSIX/packed-struct header instead), so a
# Makefile-style generator that actually honours CMAKE_C_COMPILER is
# required.
if (Test-Path -LiteralPath $buildDir) {
    Remove-Item -Recurse -Force -LiteralPath $buildDir
}
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

# CMAKE_SYSTEM_NAME/PROCESSOR are what actually puts CMake into cross-compile
# mode -- without them it still treats the link step as "build a Windows PE
# with a MinGW-hosted compiler" and passes DLL-only flags
# (--major-image-version etc) that this ARM ELF linker rejects outright.
& $cmake.Source -G "MinGW Makefiles" -B $buildDir -S $stepRoot `
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm `
    -DCMAKE_C_COMPILER="$CrossCompiler"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& $cmake.Source --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

Write-Host "Step 6 libzmpio/zmpiod/zmpioctl build PASS -> $buildDir"
Write-Host "Note: this only proves the ARM cross-build is clean (see docs/PLAN_BUOC_6.md SS4 6.3/6.4"
Write-Host "for what still needs the real board/VM: UIO runtime behavior, doorbell IRQ delivery,"
Write-Host "kill -9 recovery, group policy)."

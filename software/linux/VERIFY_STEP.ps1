$ErrorActionPreference = "Stop"
$stepRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

& (Join-Path $stepRoot "VERIFY_UPSTREAM.ps1")
& (Join-Path $stepRoot "BUILD_STEP.ps1")

Write-Host ""
Write-Host "Step 6 full local verification PASS"
Write-Host "This proves: common/ ABI headers unchanged since capture, and libzmpio/zmpiod/zmpioctl"
Write-Host "cross-compile cleanly for ARM Linux. It does NOT prove any board/UIO/doorbell runtime"
Write-Host "behavior -- see docs/PLAN_BUOC_6.md SS4 for what still needs the physical board."

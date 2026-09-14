param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$SkipGolden
)

$ErrorActionPreference = "Stop"
$stepRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $stepRoot "build"
$goldenPath = Join-Path $stepRoot "golden\golden_tone_features.csv"

$compiler = Get-Command g++ -ErrorAction Stop
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$optimization = if ($Configuration -eq "Release") { "-O2" } else { "-O0" }
$commonSources = @(
    "fpga\dsp_core\src\fir3_axis.cpp",
    "fpga\dsp_core\src\fft128_real.cpp",
    "fpga\dsp_core\src\feature_extract.cpp",
    "fpga\dsp_core\src\dsp_core.cpp"
) | ForEach-Object { Join-Path $stepRoot $_ }
$commonFlags = @(
    "-std=c++17", $optimization, "-Wall", "-Wextra", "-Wpedantic", "-Werror",
    "-I", (Join-Path $stepRoot "fpga\dsp_core\src")
)

& $compiler.Source @commonFlags @commonSources `
    (Join-Path $stepRoot "fpga\dsp_core\tb\tb_dsp_core.cpp") `
    "-o" (Join-Path $buildDir "step1_tests.exe")
if ($LASTEXITCODE -ne 0) { throw "Step1 test build failed" }

& (Join-Path $buildDir "step1_tests.exe")
if ($LASTEXITCODE -ne 0) { throw "Step1 regression failed" }

& $compiler.Source @commonFlags @commonSources `
    (Join-Path $stepRoot "fpga\dsp_core\tb\gen_golden.cpp") `
    "-o" (Join-Path $buildDir "step1_golden.exe")
if ($LASTEXITCODE -ne 0) { throw "Step1 golden generator build failed" }

if (-not $SkipGolden) {
    & (Join-Path $buildDir "step1_golden.exe") $goldenPath
    if ($LASTEXITCODE -ne 0) { throw "Golden vector generation failed" }
    $goldenHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $goldenPath).Hash
    "$goldenHash  golden_tone_features.csv" | Set-Content `
        -LiteralPath (Join-Path $stepRoot "golden\manifest.sha256") `
        -Encoding ascii
}

Write-Host "Step1 host regression PASS"

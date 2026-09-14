@echo off
setlocal EnableExtensions

rem Build the CPU0 bare-metal ELF from the existing ZMPIO Vitis workspace.
rem   platform = platform_dual
rem   domain   = standalone_ps7_cortexa9_0
rem   app      = cpu0_application

rem Repository layout and toolchain locations, resolved from this
rem script's own location. See config/toolchain.env.
call "%~dp0project_paths.bat"
set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%REPO_ROOT%"
set "CPU0_BUILD_XSA=%VIVADO_XSA%"
set "CPU0_BUILD_BIT=%VIVADO_BIT%"
set "CPU0_BUILD_BIT_TARGET=%PROJECT_ROOT%\firmware\cpu0_application\_ide\bitstream\cpu_wrapper.bit"
set "CPU0_BUILD_OUTPUT=%PROJECT_ROOT%\output\cpu0"
set "CPU0_BUILD_ARGS="

:parse_args
if "%~1"=="" goto run
if /I "%~1"=="--clean" (
    set "CPU0_BUILD_ARGS=%CPU0_BUILD_ARGS% --clean"
    shift
    goto parse_args
)
if /I "%~1"=="--skip-domain-regen" (
    set "CPU0_BUILD_ARGS=%CPU0_BUILD_ARGS% --skip-domain-regen"
    shift
    goto parse_args
)
if /I "%~1"=="--output" (
    if "%~2"=="" goto missing_value
    set "CPU0_BUILD_OUTPUT=%~2"
    shift
    shift
    goto parse_args
)
echo [ERROR] Unknown option: %~1
exit /b 2

:run
if not exist "%VITIS_ROOT%\settings64.bat" (
    echo [ERROR] Vitis 2023.2 was not found at "%VITIS_ROOT%".
    exit /b 1
)

echo [INFO] Vitis root : %VITIS_ROOT%
echo [INFO] Output     : %CPU0_BUILD_OUTPUT%

echo [INFO] Cleaning up any existing Vitis/Rigel processes...
taskkill /F /IM vitis.exe 2>nul
taskkill /F /IM RigelApp.exe 2>nul
taskkill /F /IM xsct.exe 2>nul
rem timeout.exe refuses to run ("Input redirection is not supported") whenever
rem stdin is not a real console -- e.g. under non-interactive automation. ping
rem is the standard batch-file substitute: it does not touch stdin at all.
ping -n 2 127.0.0.1 >nul

echo [INFO] Removing workspace lock file if present...
if exist "%PROJECT_ROOT%\firmware\.metadata\.lock" (
    del /F /Q "%PROJECT_ROOT%\firmware\.metadata\.lock" 2>nul
)

call "%VITIS_ROOT%\settings64.bat"
if errorlevel 1 (
    echo [ERROR] Could not initialize the Vitis environment.
    exit /b 1
)

set "CPU0_BUILD_OUTPUT_ARG=%CPU0_BUILD_OUTPUT%"
call "%VITIS_ROOT%\bin\vitis.bat" -s "%SCRIPT_DIR%build_cpu0.py" --output "%CPU0_BUILD_OUTPUT_ARG%" %CPU0_BUILD_ARGS%
set "BUILD_RESULT=%ERRORLEVEL%"

if not "%BUILD_RESULT%"=="0" (
    echo [ERROR] CPU0 build failed with exit code %BUILD_RESULT%.
    exit /b %BUILD_RESULT%
)

rem Synchronize the bitstream from Vivado into the app's _ide cache.
powershell.exe -NoLogo -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $source=[IO.Path]::GetFullPath($env:CPU0_BUILD_BIT); $destination=[IO.Path]::GetFullPath($env:CPU0_BUILD_BIT_TARGET); if (-not [String]::Equals($source,$destination,[StringComparison]::OrdinalIgnoreCase)) { Copy-Item -LiteralPath $source -Destination $destination -Force }; if ((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash) { throw 'CPU0 bitstream hash verification failed' }; Write-Host '[OK] PowerShell bitstream synchronization verified.'"
if errorlevel 1 (
    echo [ERROR] PowerShell bitstream synchronization failed.
    exit /b 1
)

echo [SUCCESS] CPU0 ELF: %CPU0_BUILD_OUTPUT%\cpu0_application.elf
exit /b 0

:missing_value
echo [ERROR] %~1 requires a value.
exit /b 2

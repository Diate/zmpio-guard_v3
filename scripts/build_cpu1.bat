@echo off
setlocal EnableExtensions

rem Build the CPU1 FreeRTOS ELF from the existing ZMPIO Vitis workspace.
rem Current project layout:
rem   platform = platform_dual
rem   domain   = free_rtos (ps7_cortexa9_1)
rem   app      = app_freertos

rem Repository layout and toolchain locations, resolved from this
rem script's own location. See config/toolchain.env.
call "%~dp0project_paths.bat"
set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%REPO_ROOT%"
set "CPU1_BUILD_XSA=%VIVADO_XSA%"
set "CPU1_BUILD_BIT=%VIVADO_BIT%"
set "CPU1_BUILD_BIT_TARGET=%PROJECT_ROOT%\firmware\app_freertos\_ide\bitstream\cpu1_wrapper.bit"
set "CPU1_BUILD_OUTPUT=%PROJECT_ROOT%\output\cpu1"
set "CPU1_BUILD_APP_ONLY="
set "CPU1_BUILD_CLEAN="
set "CPU1_BUILD_DRY_RUN="

:parse_args
if "%~1"=="" goto run
if /I "%~1"=="--app-only" (
    set "CPU1_BUILD_APP_ONLY=1"
    shift
    goto parse_args
)
if /I "%~1"=="--clean" (
    set "CPU1_BUILD_CLEAN=1"
    shift
    goto parse_args
)
if /I "%~1"=="--dry-run" (
    set "CPU1_BUILD_DRY_RUN=1"
    shift
    goto parse_args
)
if /I "%~1"=="--xsa" (
    if "%~2"=="" goto missing_value
    set "CPU1_BUILD_XSA=%~2"
    shift
    shift
    goto parse_args
)
if /I "%~1"=="--bit" (
    if "%~2"=="" goto missing_value
    set "CPU1_BUILD_BIT=%~2"
    shift
    shift
    goto parse_args
)
if /I "%~1"=="--output" (
    if "%~2"=="" goto missing_value
    set "CPU1_BUILD_OUTPUT=%~2"
    shift
    shift
    goto parse_args
)
if /I "%~1"=="--help" goto usage
echo [ERROR] Unknown option: %~1
goto usage_error

:run
if not exist "%VITIS_ROOT%\settings64.bat" (
    echo [ERROR] Vitis 2023.2 was not found at "%VITIS_ROOT%".
    echo         Set VITIS_ROOT before running this file.
    exit /b 1
)

echo [INFO] Close the Vitis UI for this workspace before a full rebuild.
echo [INFO] Vitis root : %VITIS_ROOT%
echo [INFO] XSA        : %CPU1_BUILD_XSA%
echo [INFO] Bitstream  : %CPU1_BUILD_BIT%
echo [INFO] Output     : %CPU1_BUILD_OUTPUT%

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

rem Options are passed through environment variables because Vitis 2023.2's
rem vitis.bat splits quoted -s arguments containing spaces.
call "%VITIS_ROOT%\bin\vitis.bat" -s "%SCRIPT_DIR%build_cpu1.py"
set "BUILD_RESULT=%ERRORLEVEL%"

if not "%BUILD_RESULT%"=="0" (
    echo [ERROR] CPU1 build failed with exit code %BUILD_RESULT%.
    exit /b %BUILD_RESULT%
)

if defined CPU1_BUILD_DRY_RUN (
    echo [SUCCESS] Dry-run completed; no ELF was built.
    exit /b 0
)

if not defined CPU1_BUILD_APP_ONLY (
    rem build_cpu1.py already validates and synchronizes this pair.  Repeat the
    rem Copy-Item at the batch boundary so either entry point leaves the same
    rem verified CPU1 bitstream in the workspace.
    powershell.exe -NoLogo -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $source=[IO.Path]::GetFullPath($env:CPU1_BUILD_BIT); $destination=[IO.Path]::GetFullPath($env:CPU1_BUILD_BIT_TARGET); if (-not [String]::Equals($source,$destination,[StringComparison]::OrdinalIgnoreCase)) { Copy-Item -LiteralPath $source -Destination $destination -Force }; if ((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash) { throw 'CPU1 bitstream hash verification failed' }; Write-Host '[OK] PowerShell bitstream synchronization verified.'"
    if errorlevel 1 (
        echo [ERROR] PowerShell bitstream synchronization failed.
        exit /b 1
    )
)

echo [SUCCESS] CPU1 ELF: %CPU1_BUILD_OUTPUT%\app_freertos.elf
exit /b 0

:missing_value
echo [ERROR] %~1 requires a value.
goto usage_error

:usage
echo Usage:
echo   %~nx0 [--xsa ^<path^>] [--bit ^<path^>] [--output ^<dir^>] [--app-only] [--clean] [--dry-run]
echo.
echo Default: synchronize Vivado XSA/bit, then rebuild platform_dual, free_rtos and app_freertos.
echo --app-only: build only source changes in app_freertos.
echo --clean: recreate the CPU1 CMake build directory before the build.
exit /b 0

:usage_error
exit /b 2

rem Override the hardware artefacts explicitly:
rem   build_cpu1.bat --xsa "<dir>\new.xsa" --bit "<dir>\new.bit"
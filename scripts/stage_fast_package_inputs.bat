@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Cross-build the Linux user space (zmpiod, zmpioctl) on Windows, then stage
rem every fast-package input in WINDOWS_SHARE_DIR.  The Linux sync script
rem consumes these exact filenames.

rem Repository layout and toolchain locations, resolved from this
rem script's own location. See config/toolchain.env.
call "%~dp0project_paths.bat"
set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%REPO_ROOT%"

set "WINDOWS_SHARE=%WINDOWS_SHARE_DIR%"
set "CPU1_ELF=%PROJECT_ROOT%\output\cpu1\app_freertos.elf"
rem Vitis imports the hardware bitstream used by CPU1 here.  Do not point to
rem the old/nonexistent platform_dual\hw\sdt path.  The imported platform XSA
rem is named after the current Vivado top-level wrapper (design_1_wrapper.xsa),
rem not the old cpu1_wrapper.xsa name -- see CLAUDE.md
rem section 6.  The staged destination filename below is kept as
rem cpu1_wrapper.xsa for the PetaLinux side, which only cares about the
rem stable staged name, not the Vivado source's own filename.
set "CPU1_BITSTREAM=%PROJECT_ROOT%\firmware\app_freertos\_ide\bitstream\cpu1_wrapper.bit"
set "CPU1_XSA=%PROJECT_ROOT%\firmware\platform_dual\hw\design_1_wrapper.xsa"
set "DTS_TEMPLATE=%PROJECT_ROOT%\deploy\petalinux_overlay\system-user-openamp-template.dtsi"
set "CLOCK_INIT_SCRIPT=%PROJECT_ROOT%\deploy\petalinux_overlay\zmpio-enable-cpu1-clocks"
set "CLOCK_INIT_SERVICE=%PROJECT_ROOT%\deploy\petalinux_overlay\zmpio-clock-init.service"
set "FAST_PACKAGE_SCRIPT=%PROJECT_ROOT%\deploy\petalinux_overlay\script1_fast_package.sh"
set "SYNC_SCRIPT=%SCRIPT_DIR%sync_share_to_petalinux.sh"

rem The Linux client stack.  zmpiod is the single reader of the ABI v2 TX
rem ring (REQ-STR-002) and the only consumer of CPU1's feature stream;
rem zmpiod.init + zmpiod-supervise provide its sysvinit supervision, since
rem this rootfs runs sysvinit rather than systemd.
set "ZMPIOD_INIT=%PROJECT_ROOT%\deploy\petalinux_overlay\zmpiod.init"
set "ZMPIOD_SUPERVISE=%PROJECT_ROOT%\deploy\petalinux_overlay\zmpiod-supervise"
set "ZMPIO_UDEV_RULES=%PROJECT_ROOT%\deploy\petalinux_overlay\zmpio-uio.rules"
set "ZMPIOD_BINARY=%PROJECT_ROOT%\output\linux\zmpiod"
set "ZMPIOCTL_BINARY=%PROJECT_ROOT%\output\linux\zmpioctl"
set "LINUX_IPC_DIR=%PROJECT_ROOT%\software\linux"
set "ZMPIO_INCLUDES=-I%PROJECT_ROOT%\common -I%LINUX_IPC_DIR%\libzmpio\include -I%LINUX_IPC_DIR%\libzmpio\src -I%LINUX_IPC_DIR% -I%LINUX_IPC_DIR%\zmpiod"

rem ARM_LINUX_CC comes from config/toolchain.env; override it in the
rem environment to use a different cross compiler.

set "STAGE_ONLY="
set "DRY_RUN="

:parse_args
if "%~1"=="" goto validate
if /I "%~1"=="--stage-only" (
    set "STAGE_ONLY=1"
    shift
    goto parse_args
)
if /I "%~1"=="--dry-run" (
    set "DRY_RUN=1"
    shift
    goto parse_args
)
if /I "%~1"=="--help" goto usage
echo [ERROR] Unknown option: %~1
goto usage_error

:validate
for %%F in ("%CPU1_ELF%" "%CPU1_BITSTREAM%" "%CPU1_XSA%" "%DTS_TEMPLATE%" "%CLOCK_INIT_SCRIPT%" "%CLOCK_INIT_SERVICE%" "%ZMPIOD_INIT%" "%ZMPIOD_SUPERVISE%" "%ZMPIO_UDEV_RULES%" "%FAST_PACKAGE_SCRIPT%" "%SYNC_SCRIPT%") do (
    if not exist "%%~fF" (
        echo [ERROR] Required file was not found: %%~fF
        exit /b 1
    )
)

if not defined STAGE_ONLY if not exist "%ARM_LINUX_CC%" (
    echo [ERROR] ARM Linux compiler was not found: %ARM_LINUX_CC%
    echo         Set ARM_LINUX_CC to arm-linux-gnueabihf-gcc.exe and run again.
    exit /b 1
)

if defined DRY_RUN (
    echo [INFO] Dry-run successful.
    echo [INFO] CPU1 ELF     : %CPU1_ELF%
    echo [INFO] CPU1 bit     : %CPU1_BITSTREAM%
    echo [INFO] CPU1 XSA     : %CPU1_XSA%
    echo [INFO] Share        : %WINDOWS_SHARE%
    exit /b 0
)

if not defined STAGE_ONLY (
    if not exist "%PROJECT_ROOT%\output\linux" mkdir "%PROJECT_ROOT%\output\linux"
    rem zmpiod and zmpioctl are compiled straight from source here rather than
    rem through their CMake project: this script only has to produce two staged
    rem binaries with the Windows Vitis cross compiler, and a second build
    rem system in the middle would only add a generator dependency.
    rem software/linux/CMakeLists.txt stays the build used on the PetaLinux VM
    rem and for the host tests.
    echo [INFO] Building zmpiod...
    "%ARM_LINUX_CC%" -std=gnu11 -O2 -Wall -Wextra %ZMPIO_INCLUDES% -o "%ZMPIOD_BINARY%" ^
        "%LINUX_IPC_DIR%\zmpiod\zmpiod.c" ^
        "%LINUX_IPC_DIR%\zmpiod\zmpio_capture.c" ^
        "%LINUX_IPC_DIR%\zmpiod\zmpio_stats.c" ^
        "%LINUX_IPC_DIR%\libzmpio\src\libzmpio.c" ^
        "%LINUX_IPC_DIR%\libzmpio\src\zmpio_uio.c" ^
        "%LINUX_IPC_DIR%\libzmpio\src\zmpio_v2.c" ^
        "%LINUX_IPC_DIR%\libzmpio\src\zmpio_v3.c" ^
        "%PROJECT_ROOT%\common\zmpio_crc32.c"
    if errorlevel 1 (
        echo [ERROR] zmpiod build failed.
        exit /b 1
    )

    echo [INFO] Building zmpioctl...
    "%ARM_LINUX_CC%" -std=gnu11 -O2 -Wall -Wextra %ZMPIO_INCLUDES% -o "%ZMPIOCTL_BINARY%" ^
        "%LINUX_IPC_DIR%\zmpioctl\zmpioctl.c"
    if errorlevel 1 (
        echo [ERROR] zmpioctl build failed.
        exit /b 1
    )
)

for %%F in ("%ZMPIOD_BINARY%" "%ZMPIOCTL_BINARY%") do (
    if not exist "%%~fF" (
        echo [ERROR] Linux binary was not found: %%~fF
        echo         Build it first, or use the default command without --stage-only.
        exit /b 1
    )
)

if not exist "%WINDOWS_SHARE%" mkdir "%WINDOWS_SHARE%"
if not exist "%WINDOWS_SHARE%" (
    echo [ERROR] Cannot create or access: %WINDOWS_SHARE%
    exit /b 1
)

call :replace "%CPU1_ELF%" "app_freertos.elf" || exit /b 1
call :replace "%CPU1_BITSTREAM%" "cpu1_bitstream.bit" || exit /b 1
call :replace "%CPU1_XSA%" "cpu1_wrapper.xsa" || exit /b 1
call :replace "%DTS_TEMPLATE%" "system-user-openamp-template.dtsi" || exit /b 1
call :replace "%CLOCK_INIT_SCRIPT%" "zmpio-enable-cpu1-clocks" || exit /b 1
call :replace "%CLOCK_INIT_SERVICE%" "zmpio-clock-init.service" || exit /b 1
call :replace "%ZMPIOD_BINARY%" "zmpiod" || exit /b 1
call :replace "%ZMPIOCTL_BINARY%" "zmpioctl" || exit /b 1
call :replace "%ZMPIOD_INIT%" "zmpiod.init" || exit /b 1
call :replace "%ZMPIOD_SUPERVISE%" "zmpiod-supervise" || exit /b 1
call :replace "%ZMPIO_UDEV_RULES%" "zmpio-uio.rules" || exit /b 1
call :replace "%FAST_PACKAGE_SCRIPT%" "script1_fast_package.sh" || exit /b 1
call :replace "%SYNC_SCRIPT%" "sync_share_to_petalinux.sh" || exit /b 1

echo [SUCCESS] Staged fast-package inputs in %WINDOWS_SHARE%
echo [NEXT] On Linux run: ./sync_share_to_petalinux.sh ^(source defaults to /media/sf_share^)
exit /b 0

:replace
set "SOURCE_FILE=%~1"
set "TARGET_FILE=%WINDOWS_SHARE%\%~2"
if exist "%TARGET_FILE%" del /f /q "%TARGET_FILE%" >nul 2>&1
copy /y "%SOURCE_FILE%" "%TARGET_FILE%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy %SOURCE_FILE% to %TARGET_FILE%
    exit /b 1
)
echo [INFO] Staged %~2
exit /b 0

:usage
echo Usage:
echo   %~nx0 [--stage-only] [--dry-run]
echo.
echo Default: build zmpiod and zmpioctl with the Windows Vitis cross
echo          compiler, then copy app_freertos.elf,
echo          cpu1_bitstream.bit, cpu1_wrapper.xsa, the DTS template, CPU1
echo          clock-init files, zmpiod, zmpioctl, the zmpiod
echo          sysvinit scripts, the UIO udev rule, and both scripts to the share.
echo --stage-only: reuse the existing output\linux\ binaries.
echo --dry-run: validate inputs and show paths without building or copying.
exit /b 0

:usage_error
exit /b 2

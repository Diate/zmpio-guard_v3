@echo off
rem Repository layout and external toolchain locations, for batch callers.
rem
rem Call it, do not run it standalone:
rem     call "%~dp0project_paths.bat"
rem
rem Every in-tree path is derived from this file's own location. Only
rem config\toolchain.env holds absolute paths, and a variable that is already
rem set always wins over the value in that file.

for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"

set "CONFIG_DIR=%REPO_ROOT%\config"
set "TOOLCHAIN_ENV=%CONFIG_DIR%\toolchain.env"

set "COMMON_DIR=%REPO_ROOT%\common"
set "DOCS_DIR=%REPO_ROOT%\docs"
set "FIRMWARE_DIR=%REPO_ROOT%\firmware"
set "CPU0_APP_DIR=%FIRMWARE_DIR%\cpu0_application"
set "CPU1_APP_DIR=%FIRMWARE_DIR%\app_freertos"
set "PLATFORM_DIR=%FIRMWARE_DIR%\platform_dual"
set "HARDWARE_DIR=%REPO_ROOT%\hardware"
set "RTL_DIR=%HARDWARE_DIR%\rtl"
set "VIVADO_PROJECT_DIR=%HARDWARE_DIR%\vivado\project_hub"
set "LINUX_SW_DIR=%REPO_ROOT%\software\linux"
set "DSP_HOST_SIM_DIR=%REPO_ROOT%\dsp_host_sim"
set "DEPLOY_DIR=%REPO_ROOT%\deploy\petalinux_overlay"
set "SCRIPTS_DIR=%REPO_ROOT%\scripts"
set "TOOLS_DIR=%REPO_ROOT%\tools"
set "ARTIFACTS_DIR=%REPO_ROOT%\artifacts"
set "OUTPUT_DIR=%REPO_ROOT%\output"

set "VIVADO_XSA=%VIVADO_PROJECT_DIR%\design_1_wrapper.xsa"
set "VIVADO_BIT=%VIVADO_PROJECT_DIR%\project_hub.runs\impl_1\design_1_wrapper.bit"

rem Load toolchain.env without clobbering anything already set.
if not exist "%TOOLCHAIN_ENV%" goto :pp_done
for /f "usebackq eol=# tokens=1,* delims==" %%A in ("%TOOLCHAIN_ENV%") do (
    if not "%%~A"=="" if not defined %%A set "%%A=%%B"
)
:pp_done
exit /b 0

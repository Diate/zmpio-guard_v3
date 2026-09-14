@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0BUILD_STEP.ps1" %*
exit /b %ERRORLEVEL%

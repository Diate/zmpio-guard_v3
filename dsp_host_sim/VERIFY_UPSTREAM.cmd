@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0VERIFY_UPSTREAM.ps1" %*
exit /b %ERRORLEVEL%


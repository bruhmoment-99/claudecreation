@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0debug.ps1" %*
exit /b %ERRORLEVEL%

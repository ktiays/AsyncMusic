@echo off
rem Takes a screenshot of the desktop session from ssh: screenshot-remote.cmd [output.png]
set OUT=%~1
if "%OUT%"=="" set OUT=%USERPROFILE%\screen.png
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-interactive.ps1" -Command "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File %~dp0screenshot.ps1 -Path %OUT%" -Wait -Timeout 30

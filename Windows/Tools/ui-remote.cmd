@echo off
rem Drives the running app from ssh: ui-remote.cmd invoke Join as Speaker | ui-remote.cmd dump
chcp 65001 >nul
echo %*> "%USERPROFILE%\ui-request.txt"
del "%USERPROFILE%\ui.log" 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-interactive.ps1" -Command "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File %~dp0ui.ps1" -Wait -Timeout 30
type "%USERPROFILE%\ui.log"

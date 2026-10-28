@echo off
rem PS2 Native Recomp: double-click this file to open the app.
rem It starts the PowerShell script app\app.ps1 next to it. The execution policy is bypassed for this one start only
rem (nothing on the PC is changed), so it also works when the folder came from a zip download.
setlocal
set "APP=%~dp0app\app.ps1"
if not exist "%APP%" echo Cannot find app\app.ps1 next to Start.cmd. Extract the whole zip file first, then start Start.cmd from the extracted folder.
if not exist "%APP%" pause
if not exist "%APP%" exit /b 1
start "" "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File "%APP%"

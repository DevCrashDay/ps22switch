@echo off
setlocal enabledelayedexpansion
rem ps22switch: drop a PS2 disc image onto this file (or give its path) to build a Nintendo Switch .nro of it.
rem Extra options go to scripts\switch_port.py, e.g. Switch_Build.cmd "Tekken 4 (USA).iso" --widescreen
cd /d "%~dp0"

echo =========================================================
echo  ps22switch: Recompiling PS2 Games for Nintendo Switch 1
echo =========================================================

set "TARGET=%~1"
if "%TARGET%"=="" (
    for %%f in ("*.iso") do (
        set "TARGET=%%~ff"
        goto :found_iso
    )
)

:found_iso
if "%TARGET%"=="" (
    echo [ERROR] No PS2 .iso file specified or found in this folder!
    echo Usage: Switch_Build.cmd ^<Game.iso^> [options]
    echo Or drop a .iso file directly onto this script.
    pause
    exit /b 1
)

rem the Python launcher when there is one, else python on the PATH (only one of them runs the build)
set "PY=python"
where py >nul 2>nul && set "PY=py -3"

echo Selected target: %TARGET%
shift
set "REST="
:collect
if "%~1"=="" goto :run
set "REST=!REST! %1"
shift
goto :collect

:run
%PY% scripts\switch_port.py "%TARGET%" !REST!
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed! See output above.
    pause
    exit /b 1
)

echo.
echo =========================================================
echo  Build Complete! The SD card folder is in build\switch
echo =========================================================
pause

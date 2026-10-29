@echo off
setlocal enabledelayedexpansion

echo =========================================================
echo  ps22switch: Recompiling PS2 Games for Nintendo Switch 1
echo =========================================================

set TARGET=%~1
if "%TARGET%"=="" (
    for %%f in ("*.iso") do (
        set TARGET=%%f
        goto :found_iso
    )
)

:found_iso
if "%TARGET%"=="" (
    echo [ERROR] No PS2 .iso file specified or found in this folder!
    echo Usage: Switch_Build.cmd ^<Game.iso^>
    echo Or drop a .iso file directly onto this script.
    pause
    exit /b 1
)

echo Selected target: %TARGET%
py -3 scripts\switch_port.py "%TARGET%"
if errorlevel 1 (
    python scripts\switch_port.py "%TARGET%"
)

if errorlevel 1 (
    echo.
    echo [ERROR] Build failed! See output above.
    pause
    exit /b 1
)

echo.
echo =========================================================
echo  Build Complete! The Switch .nro is ready in build\switch
echo =========================================================
pause

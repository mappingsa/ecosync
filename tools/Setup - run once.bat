@echo off
REM ==========================================================================
REM  FPE Field Reporting - one-off setup for a new computer.
REM
REM  Needs Python 3.9 or newer from python.org, installed with
REM  "Add python.exe to PATH" ticked. Safe to run again at any time.
REM ==========================================================================

setlocal
cd /d "%~dp0"
title FPE Field Reporting - Setup

echo.
echo   Checking Python...
where python >nul 2>&1
if errorlevel 1 goto :nopython
python -c "import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)" >nul 2>&1
if errorlevel 1 goto :oldpython
python --version

echo.
echo   Installing the packages the scripts use...
echo   (requests, shapely, and tzdata for South Australian time)
python -m pip install --upgrade --quiet requests shapely tzdata
if errorlevel 1 goto :pipfail

python -c "import requests, shapely; from zoneinfo import ZoneInfo; ZoneInfo('Australia/Adelaide')" >nul 2>&1
if errorlevel 1 goto :pipfail
echo   Packages OK.

echo.
if exist "spray_stats_config.json" (
  echo   Login file found - spray_stats_config.json.
) else (
  echo   STILL NEEDED: spray_stats_config.json
  echo   Jayme will copy it into this folder. It holds the ArcGIS login,
  echo   so do not email it or put it in Dropbox.
)

echo.
echo   Setup finished. Use "End of Day.bat" from now on.
echo.
pause
exit /b 0

:nopython
echo.
echo   Python is not installed, or is not on PATH.
echo.
echo   1. Download Python from https://www.python.org/downloads/
echo   2. In the installer, TICK "Add python.exe to PATH"
echo   3. Run this setup again.
echo.
pause
exit /b 1

:oldpython
echo.
echo   This Python is too old - 3.9 or newer is needed.
echo   Install the current version from https://www.python.org/downloads/
echo.
pause
exit /b 1

:pipfail
echo.
echo   The packages did not install. Check the internet connection and run
echo   this again. If it still fails, send Jayme a photo of this window.
echo.
pause
exit /b 1

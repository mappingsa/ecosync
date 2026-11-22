@echo off
title EcoSync - import from ArcGIS
cd /d "%~dp0"
where python >nul 2>&1
if errorlevel 1 (
  echo Python is not installed or not on PATH - run EcoSync\tools\Setup - run once.bat first.
  pause
  exit /b 1
)
echo Copying the last 14 days from ArcGIS to the EcoSync Data Platform...
echo (add --full after the file name to copy everything)
echo.
python import_arcgis.py %*
echo.
pause

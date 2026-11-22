@echo off
title EcoSync - set up hourly import
cd /d "%~dp0"
where pythonw >nul 2>&1
if errorlevel 1 (
  echo Python is not installed or not on PATH - run EcoSync\tools\Setup - run once.bat first.
  pause
  exit /b 1
)
for /f "delims=" %%P in ('where pythonw') do set "PYW=%%P" & goto :found
:found
rem Runs every hour while you are signed in to Windows, with no window.
rem Results go to import_log.txt in this folder.
schtasks /Create /F /SC HOURLY /TN "EcoSync ArcGIS import" /TR "\"%PYW%\" \"%~dp0import_arcgis.py\""
if errorlevel 1 (
  echo Could not create the scheduled task - see the message above.
) else (
  echo Done. The import now runs every hour. To stop it:
  echo   schtasks /Delete /TN "EcoSync ArcGIS import" /F
)
pause

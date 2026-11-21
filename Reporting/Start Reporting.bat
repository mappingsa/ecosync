@echo off
title EcoSync Reporting
cd /d "%~dp0"

where node >nul 2>nul
if errorlevel 1 (
  echo Node.js is not installed. Install the LTS version from https://nodejs.org and run this again.
  pause
  exit /b 1
)

rem Installs any new packages (quick when nothing has changed).
echo Checking packages...
call npm install --no-audit --no-fund
if errorlevel 1 (
  echo npm install failed - see the messages above.
  pause
  exit /b 1
)

echo Starting EcoSync Reporting at https://localhost:5173  (close this window to stop it)
start "" https://localhost:5173
call npm run dev
pause

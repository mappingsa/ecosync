@echo off
title EcoSync Reporting - build for publishing
cd /d "%~dp0"
echo Checking packages...
call npm install --no-audit --no-fund
if errorlevel 1 goto failed
echo Building the site...
call npm run build
if errorlevel 1 goto failed
echo.
echo Done. Upload everything inside the "dist" folder to Cloudflare Pages
echo (project: ecosync-reporting).
start "" "%~dp0dist"
start "" https://dash.cloudflare.com/?to=/:account/pages
pause
exit /b 0
:failed
echo Build failed - see the messages above.
pause
exit /b 1

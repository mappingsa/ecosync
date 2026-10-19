@echo off
REM ==========================================================================
REM  MSA EcoSync - fetch the ArcGIS root CA as ca.pem for the unit's SD card.
REM
REM  Double-click this, NOT the .ps1. Running the .ps1 directly opens a window
REM  that closes the instant it finishes, so any error scrolls past unread.
REM
REM  Everything printed is also written to get_ca_pem_log.txt beside this file,
REM  so the output survives even if the window does not.
REM ==========================================================================

setlocal
cd /d "%~dp0"

where powershell >nul 2>&1
if errorlevel 1 (
  echo [FATAL] powershell is not on PATH.
  echo.
  pause
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0get_ca_pem.ps1"

echo.
echo ---------------------------------------------------------------------
echo A copy of the above is in: %~dp0get_ca_pem_log.txt
echo.
pause

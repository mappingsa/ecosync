@echo off
REM ==========================================================================
REM  MSA EcoSync - make a shareable copy of a unit's config.txt
REM
REM  DRAG the SD card's config.txt onto this file.
REM
REM  It writes config_redacted.txt beside this script, with the Wi-Fi password,
REM  ArcGIS token and device login replaced by a note saying only whether they
REM  are set. Everything else is copied exactly, comments and all.
REM
REM  With no file dropped it looks for config.txt in this folder.
REM ==========================================================================

setlocal
cd /d "%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0redact_config.ps1" %1

echo.
pause

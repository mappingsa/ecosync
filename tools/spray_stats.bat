@echo off
REM ==========================================================================
REM  MSA EcoSync - prepare the daily spray report.
REM
REM  Typical end of day, after submitting the Survey123 form:
REM
REM      spray_stats.bat 2026-09-19 --setextent --write
REM
REM  That frames the report's web map on the day's data AND fills the accuracy
REM  figures into the record, so the report comes out right the first time.
REM
REM  Safer first run - look before it touches anything:
REM
REM      spray_stats.bat 2026-09-19 --showextent
REM
REM  Other forms:
REM      spray_stats.bat                  today, print the figures only
REM      spray_stats.bat 2026-09-19       that day, print only
REM
REM  --setextent edits a live web map. It writes a backup of the web map JSON
REM  beside this script first, every time, named webmap_backup_<id>_<stamp>.json
REM ==========================================================================

setlocal
cd /d "%~dp0"

where python >nul 2>&1
if errorlevel 1 (
  echo [FATAL] python is not on PATH.
  pause
  exit /b 1
)

python spray_stats.py %*
echo.
pause

@echo off
REM ==========================================================================
REM  MSA EcoSync - end of field day.
REM
REM  Replaces the Runs.txt checklist. Finds the day's jobs itself, builds the
REM  treated-area polygons, then runs the statistics once per job.
REM
REM  Look first, write nothing:
REM
REM      end_of_day.bat
REM
REM  Then, when the output looks right:
REM
REM      end_of_day.bat --write
REM
REM  Other forms:
REM      end_of_day.bat 2026-10-14            that day, dry run
REM      end_of_day.bat 2026-10-14 --write    that day, for real
REM      end_of_day.bat --yesterday --write   processing the next morning
REM
REM  Options:
REM      --no-extent   do not reframe the web map
REM      --no-track    skip the polygon build
REM      --no-stats    skip the statistics
REM
REM  Every run appends a line to end_of_day_log.txt, so there is a record of
REM  when each day's data was processed.
REM ==========================================================================

setlocal
cd /d "%~dp0"

where python >nul 2>&1
if errorlevel 1 (
  echo [FATAL] python is not on PATH.
  pause
  exit /b 1
)

python end_of_day.py %*
echo.
pause

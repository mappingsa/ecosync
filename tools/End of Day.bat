@echo off
REM ==========================================================================
REM  FPE Field Reporting - End of Day, guided.
REM
REM  Double-click. It asks for the date, checks the Form 1 record and job are
REM  ready, does a dry run, and only saves after you press Y.
REM
REM  Wraps end_of_day.py - the same script as end_of_day.bat - and adds:
REM    - a pre-flight check (check_before_end_of_day.py), because the Form 1
REM      record is otherwise only looked for during the write;
REM    - the date prompt, so nobody types --write or a date by hand;
REM    - a stop if the dry run reports any problem.
REM ==========================================================================

setlocal
cd /d "%~dp0"
title FPE Field Reporting - End of Day

where python >nul 2>&1
if errorlevel 1 goto :nopython
if not exist "spray_stats_config.json" goto :noconfig

echo.
echo  ======================================================================
echo   FPE Field Reporting - End of Day
echo  ======================================================================
echo.
echo   Before you start, the Form 1 spray record for the day must already be
echo   submitted, with the job number exactly as it is on the EcoSync unit.
echo.
set "DAY="
set /p "DAY=  Date to process as YYYY-MM-DD, or just press Enter for today: "

echo.
echo  ----------------------------------------------------------------------
echo   STEP 1 of 3 - Is everything ready?   (read only)
echo  ----------------------------------------------------------------------
python check_before_end_of_day.py %DAY%
set "RC=%ERRORLEVEL%"
if "%RC%"=="2" goto :baddate
if not "%RC%"=="0" goto :notready

echo  ----------------------------------------------------------------------
echo   STEP 2 of 3 - Dry run   (nothing is changed)
echo  ----------------------------------------------------------------------
python end_of_day.py %DAY%
set "RC=%ERRORLEVEL%"
if "%RC%"=="2" goto :baddate
if not "%RC%"=="0" goto :problems

echo.
echo   Check the output above:
echo     - "Jobs that day" lists the job or jobs you worked
echo     - the record counts look about right for the day
echo     - it ends with "Finished."
echo.
choice /c YN /n /m "  Does it look right? Press Y to save it, or N to stop: "
if errorlevel 2 goto :stopped

echo.
echo  ----------------------------------------------------------------------
echo   STEP 3 of 3 - Saving
echo  ----------------------------------------------------------------------
python end_of_day.py %DAY% --write
if errorlevel 1 goto :problems

echo.
echo  ======================================================================
echo   Done. Now generate the report on the Survey123 website.
echo  ======================================================================
echo.
pause
exit /b 0

:nopython
echo.
echo   Python is not set up on this computer.
echo   Run "Setup - run once.bat" in this folder first.
echo.
pause
exit /b 1

:noconfig
echo.
echo   spray_stats_config.json is missing from this folder.
echo   Ask Jayme for it - it holds the ArcGIS login.
echo.
pause
exit /b 1

:baddate
echo.
echo   That date was not understood. Run this again and type it as
echo   YYYY-MM-DD, for example 2026-10-28, or just press Enter for today.
echo.
pause
exit /b 2

:notready
echo.
echo   Not ready - NOTHING has been changed.
echo   Fix what is marked [X] above, then run this again.
echo.
pause
exit /b 1

:problems
echo.
echo   ** Something went wrong. Do not generate the report yet. **
echo   Take a photo of this window and send it to Jayme.
echo.
pause
exit /b 1

:stopped
echo.
echo   Stopped - nothing was saved.
echo.
pause
exit /b 0

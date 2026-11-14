@echo off
REM ==========================================================================
REM  MSA EcoSync - copy the reporting scripts into the field reporting pack.
REM
REM  The scripts are maintained HERE, in EcoSync\tools (under git). The pack
REM  at ArcGIS\FPE Field Reporting is what gets copied to other computers.
REM  Run this after changing any of the reporting scripts, then copy the
REM  pack across to the other computer again.
REM
REM  Never copies spray_stats_config.json - that is done by hand.
REM ==========================================================================

setlocal
cd /d "%~dp0"
set "PACK=%USERPROFILE%\ArcGIS\FPE Field Reporting"

if not exist "%PACK%\" (
  echo.
  echo   Pack folder not found: %PACK%
  echo.
  pause
  exit /b 1
)

for %%F in (end_of_day.py spray_stats.py track_to_polygons.py check_before_end_of_day.py spray_stats_config.example.json "End of Day.bat" "Setup - run once.bat") do (
  copy /y "%%~F" "%PACK%\" >nul
  if errorlevel 1 (echo   FAILED  %%~F) else (echo   copied  %%~F)
)

echo.
echo   Pack updated. Copy the "FPE Field Reporting" folder to the other
echo   computer again - but keep its own spray_stats_config.json.
echo.
pause

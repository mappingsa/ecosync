@echo off
REM ==========================================================================
REM  MSA EcoSync - build treated-area polygons from continuous track points.
REM
REM  Look first, always:
REM
REM      track_to_polygons.bat 2026-09-27
REM
REM  That posts NOTHING. It prints the run and area figures and writes
REM  track_polygons\track_polygons_<date>.geojson - drag that onto a map
REM  and check the shapes look like the ground you covered.
REM
REM  When it looks right:
REM
REM      track_to_polygons.bat 2026-09-27 --write
REM
REM  Other forms:
REM      track_to_polygons.bat                          today, dry run
REM      track_to_polygons.bat 2026-09-27 --buffer 2.0  wider spray swath
REM      track_to_polygons.bat 2026-09-27 --no-merge    one polygon per run
REM
REM  --buffer is a RADIUS in metres and it decides the area you report, so it
REM  is stored on every polygon this produces. Default is 1.5 m.
REM
REM  Needs: pip install requests shapely
REM ==========================================================================

setlocal
cd /d "%~dp0"

where python >nul 2>&1
if errorlevel 1 (
  echo [FATAL] python is not on PATH.
  pause
  exit /b 1
)

python -c "import shapely" >nul 2>&1
if errorlevel 1 (
  echo [FATAL] shapely is not installed. Run:
  echo.
  echo     pip install requests shapely
  echo.
  pause
  exit /b 1
)

python track_to_polygons.py %*
echo.
pause

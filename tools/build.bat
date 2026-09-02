@echo off
REM ==========================================================================
REM  MSA EcoSync - compile
REM
REM  Double-click this file. Output goes to build_log.txt in the repo root.
REM
REM  The sketch now lives at firmware\ecosync_field\ecosync_field.ino. Because
REM  the folder and the file share a name, arduino-cli accepts it directly and
REM  nothing has to be COPIED anywhere. That copy step is gone deliberately: it
REM  was a second copy of the source that could silently drift from the first.
REM
REM  Optional: pass a board FQBN to override the default.
REM      build.bat esp32:esp32:esp32thing_plus_c
REM ==========================================================================

setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "SKETCH=%ROOT%\firmware\ecosync_field"
set "LOG=%ROOT%\build_log.txt"
set "FQBN=%~1"
if "%FQBN%"=="" set "FQBN=esp32:esp32:esp32thing_plus_c"

if not exist "%SKETCH%\ecosync_field.ino" (
  echo.
  echo [FATAL] Cannot find "%SKETCH%\ecosync_field.ino"
  echo         Run this from the tools folder inside the repo.
  echo.
  pause
  exit /b 1
)

echo.
echo   Sketch : firmware\ecosync_field
echo   Board  : %FQBN%
echo   Log    : build_log.txt
echo.
echo Building, please wait...
call :main > "%LOG%" 2>&1
echo.
if errorlevel 1 (echo BUILD FAILED - see build_log.txt) else (echo BUILD OK - see build_log.txt)
echo.
pause
exit /b

REM --------------------------------------------------------------------------
:main
echo ==========================================================
echo  MSA EcoSync build log
echo  Date  : %DATE% %TIME%
echo  Sketch: firmware\ecosync_field
echo  Board : %FQBN%
echo ==========================================================
echo.

where arduino-cli >nul 2>&1
if errorlevel 1 (
  echo [FATAL] arduino-cli is not on PATH.
  echo   winget install ArduinoSA.CLI
  exit /b 1
)

echo --- arduino-cli version ---
arduino-cli version
echo.

echo --- Updating board index ---
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
echo.
echo --- Installed cores ---
arduino-cli core list
echo.

arduino-cli core list | findstr /I "esp32:esp32" >nul
if errorlevel 1 (
  echo --- Installing esp32 core ^(large download, be patient^) ---
  arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
  echo.
)

REM Libraries PINNED to the versions this firmware was written against.
echo --- Libraries ---
arduino-cli lib update-index
arduino-cli lib install "SparkFun u-blox GNSS Arduino Library@2.2.29"
arduino-cli lib install "SparkFun MAX1704x Fuel Gauge Arduino Library@1.0.4"
echo.
arduino-cli lib list
echo.

echo ==========================================================
echo  COMPILING
echo ==========================================================
arduino-cli compile --fqbn %FQBN% --warnings all "%SKETCH%"
set "RC=%ERRORLEVEL%"
echo.
echo ==========================================================
if "%RC%"=="0" (echo  RESULT: SUCCESS) else (echo  RESULT: FAILED ^(exit code %RC%^))
echo ==========================================================
exit /b %RC%

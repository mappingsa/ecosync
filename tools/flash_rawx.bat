@echo off
REM ==========================================================================
REM  MSA EcoSync - flash the RAWX / carrier-phase TEST sketch
REM
REM  THIS REPLACES THE FIELD FIRMWARE ON THE UNIT.
REM  When you are done, run flash.bat to put the real firmware back.
REM
REM  Nothing on the SD card is touched, and the test writes nothing to the
REM  GNSS module's flash - its settings are RAM-only and clear on power cycle.
REM
REM  Usage:
REM      flash_rawx.bat          try COM9 first, then anything else attached
REM      flash_rawx.bat COM3     go straight to that port
REM ==========================================================================

setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "SKETCH=%ROOT%\firmware\rawx_test"
set "LOG=%ROOT%\flash_rawx_log.txt"
set "OUTDIR=%ROOT%\build_out_rawx"
set "FORCEPORT=%~1"
set "DEFAULTPORT=COM9"
set "FQBN=esp32:esp32:esp32thing_plus_c"
set "PORTFILE=%TEMP%\msa_ports_rawx.txt"

if not exist "%SKETCH%\rawx_test.ino" (
  echo [FATAL] Cannot find "%SKETCH%\rawx_test.ino"
  pause
  exit /b 1
)

echo.
echo   ************************************************************
echo    TEST FIRMWARE - this replaces the field firmware.
echo    Run flash.bat afterwards to put EcoSync back.
echo   ************************************************************
echo.
echo   Sketch : firmware\rawx_test
echo   Board  : %FQBN%
echo.
echo   Close the Arduino Serial Monitor first if it is open.
echo.
echo Flashing, please wait...
call :main > "%LOG%" 2>&1
echo.
if errorlevel 1 (echo FLASH FAILED - see flash_rawx_log.txt) else (echo FLASH OK - see flash_rawx_log.txt)
echo.
echo   Now open the Serial Monitor at 115200 and take the unit outside.
echo   The test runs for about 70 seconds and prints a verdict.
echo.
echo   When finished:  flash.bat
echo.
pause
exit /b

REM --------------------------------------------------------------------------
:main
echo ==========================================================
echo  MSA EcoSync - RAWX test flash log
echo  Date  : %DATE% %TIME%
echo ==========================================================
echo.

where arduino-cli >nul 2>&1
if errorlevel 1 (
  echo [FATAL] arduino-cli is not on PATH.
  exit /b 1
)

echo --- Boards currently attached ---
arduino-cli board list
echo.

set "PORTS="
if not "%FORCEPORT%"=="" (
  set "PORTS=%FORCEPORT%"
  echo --- Port forced on the command line: %FORCEPORT% ---
  goto :haveports
)

arduino-cli board list > "%PORTFILE%" 2>&1
for /f "tokens=1" %%P in ('findstr /B /C:"COM" "%PORTFILE%"') do call :addport %%P
echo --- Ports detected:!PORTS! ---

if "!PORTS!"=="" (
  echo [WARN] Auto-detection found no COM port. Raw output was:
  type "%PORTFILE%"
  set "PORTS=COM1 COM2 COM3 COM4 COM5 COM6 COM7 COM8 COM10 COM11 COM12"
)

if not "%DEFAULTPORT%"=="" (
  set "PORTS=%DEFAULTPORT% !PORTS!"
  echo --- Trying %DEFAULTPORT% first ---
)

:haveports
echo.
if "!PORTS!"=="" goto :noport

echo ==========================================================
echo  COMPILING
echo ==========================================================
arduino-cli compile --fqbn %FQBN% --output-dir "%OUTDIR%" "%SKETCH%"
if errorlevel 1 (
  echo.
  echo [FATAL] Compile failed - not attempting any upload.
  echo         The unit keeps running whatever it had. Nothing is broken.
  exit /b 1
)
echo.

set "GOODPORT="
for %%P in (%PORTS%) do (
  if not defined GOODPORT (
    echo ==========================================================
    echo  UPLOAD ATTEMPT on %%P
    echo ==========================================================
    arduino-cli upload --fqbn %FQBN% --port %%P --input-dir "%OUTDIR%"
    if !ERRORLEVEL! EQU 0 (
      set "GOODPORT=%%P"
      echo   ^>^> %%P accepted the upload.
    ) else (
      echo   ^>^> %%P did not accept the upload, trying the next one.
    )
    echo.
  )
)

echo ==========================================================
if defined GOODPORT (
  echo  RESULT: TEST FIRMWARE FLASHED on !GOODPORT!
  echo.
  echo  Serial Monitor at 115200. Take the unit outside.
  echo  REMEMBER: run flash.bat afterwards to restore EcoSync.
  echo ==========================================================
  exit /b 0
) else (
  echo  RESULT: FAILED - no port accepted the upload
  echo    1. Close the Serial Monitor or anything holding a COM port.
  echo    2. Check the cable is a DATA USB-C cable.
  echo    3. Hold BOOT, tap RESET, release BOOT, then run this again.
  echo ==========================================================
  exit /b 1
)

:addport
set "PORTS=%PORTS% %~1"
exit /b 0

:noport
echo [FATAL] No COM port found at all.
exit /b 1

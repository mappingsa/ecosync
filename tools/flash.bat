@echo off
REM ==========================================================================
REM  MSA EcoSync - compile and upload
REM
REM  Close the Arduino Serial Monitor first. Windows gives a COM port to one
REM  program at a time, and "Access is denied" always means something else has
REM  it open.
REM
REM  Usage:
REM      flash.bat            - try each attached port until one accepts
REM      flash.bat COM3       - go straight to that port (faster, and you know
REM                             the answer: this board has been on COM3)
REM ==========================================================================

setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "SKETCH=%ROOT%\firmware\ecosync_field"
set "LOG=%ROOT%\flash_log.txt"
set "OUTDIR=%ROOT%\build_out"
set "FORCEPORT=%~1"
set "FQBN=esp32:esp32:esp32thing_plus_c"
set "PORTFILE=%TEMP%\msa_ports.txt"

if not exist "%SKETCH%\ecosync_field.ino" (
  echo [FATAL] Cannot find "%SKETCH%\ecosync_field.ino"
  pause
  exit /b 1
)

echo.
echo   Sketch : firmware\ecosync_field
echo   Board  : %FQBN%
echo.
echo   Close the Arduino Serial Monitor first if it is open.
echo.
echo Flashing, please wait...
call :main > "%LOG%" 2>&1
echo.
if errorlevel 1 (echo FLASH FAILED - see flash_log.txt) else (echo FLASH OK - see flash_log.txt)
echo.
pause
exit /b

REM --------------------------------------------------------------------------
:main
echo ==========================================================
echo  MSA EcoSync flash log
echo  Date  : %DATE% %TIME%
echo  Sketch: firmware\ecosync_field
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

REM Port detection goes via a temp FILE, not a pipe inside a for/f inside an
REM else-block. That arrangement once returned an empty list and skipped the
REM compile AND the upload without saying so - the log simply stopped.
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
  echo.
  echo [WARN] Auto-detection found no COM port. Raw output was:
  type "%PORTFILE%"
  echo.
  echo Falling back to trying COM1 through COM12.
  set "PORTS=COM1 COM2 COM3 COM4 COM5 COM6 COM7 COM8 COM9 COM10 COM11 COM12"
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
  echo         The board keeps running whatever it had. Nothing is broken.
  exit /b 1
)
echo.

REM Upload the ALREADY-BUILT binary to each port in turn, so a wrong guess
REM costs seconds rather than a full recompile.
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
  echo  RESULT: FLASHED OK on !GOODPORT!
  echo.
  echo  Next time skip the search with:  flash.bat !GOODPORT!
  echo.
  echo  Then open the Serial Monitor at 115200 and check the build stamp:
  echo    "Firmware build: ..." must match the version you just flashed.
  echo  The stamp also shows on the dashboard under the title.
  echo ==========================================================
  exit /b 0
) else (
  echo  RESULT: FAILED - no port accepted the upload
  echo.
  echo    1. Close the Serial Monitor, PuTTY, or anything holding a COM port.
  echo    2. Check the cable is a DATA USB-C cable, not charge-only.
  echo    3. Hold BOOT, tap RESET, release BOOT, then run this again.
  echo    4. Check Device Manager for a CP210x or CH340 entry.
  echo ==========================================================
  exit /b 1
)

:addport
set "PORTS=%PORTS% %~1"
exit /b 0

:noport
echo [FATAL] No COM port found at all.
exit /b 1

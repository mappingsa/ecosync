@echo off
REM ==========================================================================
REM  FPE EcoSync - compile and upload
REM
REM  Close the Arduino Serial Monitor first. Windows gives a COM port to one
REM  program at a time, and "Access is denied" always means something else has
REM  it open.
REM
REM  Usage:
REM      flash.bat            - try COM9 first (this board's usual port), then
REM                             anything else attached
REM      flash.bat COM3       - go straight to that port and try nothing else
REM ==========================================================================

setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "SKETCH=%ROOT%\firmware\ecosync_field"
set "LOG=%ROOT%\flash_log.txt"
set "OUTDIR=%ROOT%\build_out"
set "FORCEPORT=%~1"
REM This board has been enumerating as COM9. Tried first, but never the only
REM one tried - Windows reassigns COM numbers on a whim, and a hard-coded port
REM that silently stops matching is exactly the kind of thing that wastes an
REM evening. Pass a port on the command line to override.
set "DEFAULTPORT=COM9"
set "FQBN=esp32:esp32:esp32thing_plus_c"
REM A SEPARATE FQBN for the upload, so the compile above is provably untouched.
REM UploadSpeed is a board menu option, not a compile setting - the binary is
REM identical either way - but keeping them apart means a flashing problem can
REM never be confused with a build difference.
REM
REM 115200 instead of the 921600 default. Auto-reset on this board is marginal
REM once there is a switch and a flying lead hanging off EN, and a slower
REM connection tolerates a sluggish reset line far better. It costs a few
REM seconds per flash and it is the first thing to try when esptool reports
REM "Wrong boot mode detected".
set "FQBN_UPLOAD=esp32:esp32:esp32thing_plus_c:UploadSpeed=115200"
set "PORTFILE=%TEMP%\fpe_ports.txt"

if not exist "%SKETCH%\ecosync_field.ino" (
  echo [FATAL] Cannot find "%SKETCH%\ecosync_field.ino"
  pause
  exit /b 1
)

echo.
echo   Sketch : firmware\ecosync_field
echo   Board  : %FQBN%
echo   Upload : %FQBN_UPLOAD%
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
echo  FPE EcoSync flash log
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

REM The empty-list fallback MUST be tested before the usual port is prepended.
REM Prepend first and the list is never empty, so a board that enumerates on no
REM recognised port would silently get one attempt instead of twelve.
if "!PORTS!"=="" (
  echo.
  echo [WARN] Auto-detection found no COM port. Raw output was:
  type "%PORTFILE%"
  echo.
  echo Falling back to trying COM1 through COM12.
  set "PORTS=COM1 COM2 COM3 COM4 COM5 COM6 COM7 COM8 COM10 COM11 COM12"
)

REM Put the usual port at the FRONT of the queue rather than replacing it. If
REM the board is on COM9 the first attempt wins and nothing else is tried; if
REM Windows has moved it, every other candidate still gets its turn.
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
    arduino-cli upload --fqbn %FQBN_UPLOAD% --port %%P --input-dir "%OUTDIR%"
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

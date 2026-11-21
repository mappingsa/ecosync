@echo off
REM ==========================================================================
REM  FPE EcoSync - watch the serial output AND save it to a file.
REM
REM  The Arduino IDE cannot save its Serial Monitor, so a bench test that
REM  matters has to be copied out by hand - and the interesting part is always
REM  the bit that scrolled past.
REM
REM      monitor.bat              COM3, default
REM      monitor.bat COM9         another port
REM      monitor.bat COM3 pad     names the log pad_<stamp>.txt
REM
REM  Logs land in tools\serial_logs\. Ctrl-C stops it.
REM
REM  CLOSE THE ARDUINO SERIAL MONITOR FIRST - only one program can hold a COM
REM  port, and the error you get if you forget does not say so clearly.
REM ==========================================================================

setlocal
cd /d "%~dp0"

set "PORT=%~1"
REM Was COM9. The board enumerated as COM3 on the desktop with the other
REM cable (20 Nov) - a different USB lead is enough to change the number.
REM Always overridable: monitor.bat COM9
if "%PORT%"=="" set "PORT=COM3"

set "TAG=%~2"
if "%TAG%"=="" set "TAG=serial"

where arduino-cli >nul 2>&1
if errorlevel 1 (
  echo [FATAL] arduino-cli is not on PATH.
  pause
  exit /b 1
)

if not exist "serial_logs" mkdir "serial_logs"

REM Sortable timestamp, locale-independent.
for /f %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%T"
set "LOG=serial_logs\%TAG%_%STAMP%.txt"

echo.
echo   Port : %PORT%
echo   Log  : %LOG%
echo.
echo   Ctrl-C to stop.
echo.

powershell -NoProfile -Command ^
  "arduino-cli monitor -p %PORT% --config baudrate=115200 2>&1 | Tee-Object -FilePath '%LOG%'"

echo.
echo   Saved: %LOG%
echo.
pause

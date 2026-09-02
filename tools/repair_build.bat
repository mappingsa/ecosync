@echo off
REM ==========================================================================
REM  MSA EcoSync Field - toolchain repair
REM
REM  Symptom this fixes:
REM      undefined reference to `ESP'
REM      undefined reference to `EspClass::getFlashFrequencyMHz()'
REM  ...reported against core.a(chip-debug-report.cpp.o).
REM
REM  Those are the ESP32 core's own symbols, from its own Esp.cpp. The sketch
REM  never touches them. It means core.a was archived without Esp.cpp.o -
REM  normally a half-written build cache or a core install that did not
REM  fully extract.
REM
REM  Stage 1 clears the caches and rebuilds from scratch.
REM  Stage 2 (only if stage 1 fails) reinstalls the ESP32 core and retries.
REM
REM  Everything is logged to repair_log.txt in this folder.
REM ==========================================================================

setlocal enabledelayedexpansion

set "FOLDER=%~dp0"
set "SKETCH=MSA_Weed_Tracker_prod_20260820"
set "LOG=%FOLDER%repair_log.txt"
set "FQBN=%~1"
if "%FQBN%"=="" set "FQBN=esp32:esp32:esp32thing_plus_c"
set "IDX=https://espressif.github.io/arduino-esp32/package_esp32_index.json"

echo Repairing ESP32 toolchain and rebuilding...
echo Output goes to repair_log.txt
call :main > "%LOG%" 2>&1
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" (
  echo BUILD OK - see repair_log.txt
) else (
  echo STILL FAILING - send repair_log.txt to Claude
)
echo.
pause
exit /b %RC%

REM --------------------------------------------------------------------------
:main
echo ==========================================================
echo  MSA EcoSync - toolchain repair log
echo  Date : %DATE% %TIME%
echo  Board: %FQBN%
echo ==========================================================
echo.

where arduino-cli >nul 2>&1
if errorlevel 1 (
  echo [FATAL] arduino-cli is not on PATH.
  exit /b 1
)

REM ---- Diagnostics: is Esp.cpp actually on disk? ----
set "COREDIR="
for /d %%D in ("%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\*") do set "COREDIR=%%D"
echo --- Core directory ---
echo %COREDIR%
echo.

if exist "%COREDIR%\cores\esp32\Esp.cpp" (
  echo Esp.cpp     : PRESENT
) else (
  echo Esp.cpp     : *** MISSING *** - the core install is incomplete
)
if exist "%COREDIR%\cores\esp32\Esp.h" (
  echo Esp.h       : PRESENT
) else (
  echo Esp.h       : *** MISSING ***
)
if exist "%COREDIR%\cores\esp32\chip-debug-report.cpp" (
  echo chip-debug-report.cpp : PRESENT
)
echo.

echo --- Does the core define the symbols the linker wants? ---
findstr /C:"getFlashFrequencyMHz" "%COREDIR%\cores\esp32\Esp.cpp" >nul 2>&1
if errorlevel 1 (
  echo Esp.cpp does NOT define getFlashFrequencyMHz - Esp.cpp is older than the rest of the core.
) else (
  echo Esp.cpp defines getFlashFrequencyMHz - source is consistent, so this is a stale build artefact.
)
echo.

REM ---- Is Esp.cpp.o inside the archived core.a? ----
set "AR="
for /d %%T in ("%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esp-x32\*") do set "AR=%%T\bin\xtensa-esp-elf-ar.exe"
if exist "%AR%" (
  for /d %%S in ("%LOCALAPPDATA%\arduino\sketches\*") do (
    if exist "%%S\core\core.a" (
      echo --- Esp.cpp.o inside %%S\core\core.a ? ---
      "%AR%" t "%%S\core\core.a" 2>nul | findstr /I "Esp.cpp.o"
      if errorlevel 1 echo   *** Esp.cpp.o NOT in core.a - this is the fault ***
      echo.
    )
  )
)

REM ==================== STAGE 1: clean rebuild ====================
echo ==========================================================
echo  STAGE 1 - clearing caches and rebuilding
echo ==========================================================
arduino-cli cache clean
if exist "%LOCALAPPDATA%\arduino\sketches" rmdir /s /q "%LOCALAPPDATA%\arduino\sketches"
if exist "%LOCALAPPDATA%\arduino\cores"    rmdir /s /q "%LOCALAPPDATA%\arduino\cores"
echo Caches cleared.
echo.

if not exist "%FOLDER%%SKETCH%\" mkdir "%FOLDER%%SKETCH%"
copy /Y "%FOLDER%%SKETCH%.ino" "%FOLDER%%SKETCH%\%SKETCH%.ino" >nul

arduino-cli compile --clean --fqbn %FQBN% "%FOLDER%%SKETCH%"
if not errorlevel 1 (
  echo.
  echo ==========================================================
  echo  RESULT: SUCCESS after cache clean
  echo ==========================================================
  exit /b 0
)

echo.
echo Stage 1 did not fix it. Moving to stage 2.
echo.

REM ==================== STAGE 2: reinstall the core ====================
echo ==========================================================
echo  STAGE 2 - reinstalling the ESP32 core ^(large download^)
echo ==========================================================
arduino-cli core uninstall esp32:esp32
arduino-cli core update-index --additional-urls %IDX%
arduino-cli core install esp32:esp32 --additional-urls %IDX%
echo.

arduino-cli cache clean
if exist "%LOCALAPPDATA%\arduino\sketches" rmdir /s /q "%LOCALAPPDATA%\arduino\sketches"

arduino-cli compile --clean --fqbn %FQBN% "%FOLDER%%SKETCH%"
set "RC2=%ERRORLEVEL%"
echo.
echo ==========================================================
if "%RC2%"=="0" (
  echo  RESULT: SUCCESS after core reinstall
) else (
  echo  RESULT: STILL FAILING - send repair_log.txt to Claude
)
echo ==========================================================
exit /b %RC2%

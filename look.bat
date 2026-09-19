@echo off
rem ============================================================
rem  Manual look: freeze the numbers and show the per-place coordinate table.
rem
rem  ASCII ONLY (cmd.exe parses .bat with the OEM code page).
rem
rem  Usage:
rem    look.bat                 show 99.50 frozen, with the coordinate table
rem    look.bat 1234.56         show 1234.56 frozen
rem    look.bat 100.00 99.50    real = 100.00, display = 99.50 (frozen)
rem    look.bat 100.00 99.50 0  same but WITHOUT the table
rem
rem  --display freezes the display number, so nothing animates and the picture holds
rem  still. --report writes each place's 纵实际坐标 / 纵显示坐标 to the log and paints
rem  the table on the window.
rem
rem  The log is build\selftest.log. Close it from the tray menu (Esc no longer closes it).
rem ============================================================
setlocal

set "EXE=%~dp0build\dshb.exe"
if not exist "%EXE%" (
  echo [look] build\dshb.exe not found -- run build.bat first.
  exit /b 2
)

set "REAL=%~1"
set "DISP=%~2"
set "SHOWREP=%~3"

if "%REAL%"=="" set "REAL=99.50"
if "%DISP%"=="" (
  if not "%REAL%"=="" set "DISP=%REAL%"
)

if "%DEEPSEEK_API_KEY%"=="" set "DEEPSEEK_API_KEY=dummy-for-look"

set "REP=--report"
if "%SHOWREP%"=="0" set "REP="

echo [look] real=%REAL% display=%DISP% (frozen) %REP%
echo [look] the coordinate table goes to build\selftest.log and onto the window.
"%EXE%" --real=%REAL% --display=%DISP% --no-anim %REP%

@echo off
rem ============================================================
rem  Run the widget with the balance PINNED at 100.00.
rem
rem  ASCII ONLY (cmd.exe parses .bat with the OEM code page).
rem
rem  WHY this exists: while the roll animation is being designed, the number must hold
rem  still. With a value that keeps changing you cannot tell whether a difference on
rem  screen came from the animation or from a new sample.
rem
rem  Uses --fixed-amount, which feeds ONE sample at startup and then skips every data
rem  source, so the number never moves.
rem
rem  Close it from the tray menu (Esc no longer closes the widget).
rem ============================================================
setlocal

set "EXE=%~dp0build\dshb.exe"
if not exist "%EXE%" (
  echo [run-fixed] build\dshb.exe not found -- run build.bat first.
  exit /b 2
)

rem A key must exist or the widget shows the NoKey state instead of the number.
if "%DEEPSEEK_API_KEY%"=="" set "DEEPSEEK_API_KEY=dummy-for-fixed-view"

rem Optional first argument changes the pinned amount: run-fixed.bat 1234.56
set "AMOUNT=100.00"
if not "%~1"=="" set "AMOUNT=%~1"

echo [run-fixed] pinned at %AMOUNT% ; close it from the tray menu.
"%EXE%" --fixed-amount=%AMOUNT%

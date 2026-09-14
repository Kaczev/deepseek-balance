@echo off
rem ============================================================
rem  Watch the number roll, on repeat. ASCII ONLY.
rem
rem  Alternates the balance between 20.00 and 99.50 every 2 seconds
rem  so the digit rolling can be judged by eye. Close this console
rem  window (or press Esc) to stop.
rem ============================================================
setlocal
set "ROOT=%~dp0"
set "EXE=%ROOT%build\dshb.exe"
if not exist "%EXE%" (
  echo [roll] NOT FOUND: %EXE%
  echo [roll] Build first by double-clicking build.bat.
  echo.
  pause
  exit /b 2
)
echo THE WIDGET IS THE ROUNDED PANEL. Watch the number roll.
echo   balance alternates 20.00 and 99.50 every 2 seconds
echo   digits slide up/down inside one line - that is the roll
echo   close this window to stop
echo.
set DEEPSEEK_API_KEY=dummy-for-roll-view
timeout /t 2 /nobreak >nul
"%EXE%" --roll=loop
echo [roll] exited
pause
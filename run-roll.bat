@echo off
rem ============================================================
rem  Roll viewer. ASCII ONLY.
rem
rem  Shows the widget and repeats the balance jump every 2 seconds
rem  so the digit rolling can be judged by eye. Close this console
rem  window (or press Esc) to stop.
rem
rem  SIZE: the panel is 315 x 129 SCREEN PIXELS, by owner decision.
rem  (An earlier build scaled it by display DPI and produced 630 x 258
rem   on this 200% screen; that was wrong and is fixed.)
rem ============================================================
setlocal
set "ROOT=%~dp0"
set "EXE=%ROOT%build\dshb.exe"
if not exist "%EXE%" (
  echo [roll] NOT FOUND: %EXE%
  echo [roll] Build first by double-clicking build.bat.
  pause
  exit /b 2
)
echo THE WIDGET IS THE ROUNDED PANEL - 315 x 129 screen pixels.
echo   balance alternates 20.00 and 99.50 every 2 seconds
echo   each digit slides up/down inside its own line - that is the roll
echo   close this window to stop
echo.
set DEEPSEEK_API_KEY=dummy-for-roll-view
timeout /t 2 /nobreak >nul
"%EXE%" --roll=loop
echo [roll] exited
pause
@echo off
rem ============================================================
rem  Manual check launcher (ASCII ONLY -- see build.bat for why)
rem
rem  Same window as run.bat, but it does NOT close on its own.
rem  Close it from the tray menu, or by shutting this console window.
rem
rem  This is "the widget": a rounded translucent panel, no border,
rem  no title bar, with a white square sliding inside it.
rem ============================================================
setlocal
set "ROOT=%~dp0"
set "EXE=%ROOT%build\dshb.exe"

if not exist "%EXE%" (
  echo.
  echo [run-long] NOT FOUND: %EXE%
  echo [run-long] Build first by double-clicking build.bat.
  echo.
  pause
  exit /b 2
)

echo THE WIDGET IS THE ROUNDED PANEL THAT IS ABOUT TO APPEAR.
echo.
echo   - it has no border and no title bar
echo   - a white square slides inside it
echo   - close it from the tray menu, or close this console window
echo.
echo Keep this console window open while you test.
echo.
timeout /t 2 /nobreak >nul

"%EXE%"

echo.
echo [run-long] widget exited with code %ERRORLEVEL%
echo [run-long] diagnostics: %ROOT%build\selftest.log
echo.
pause

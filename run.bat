@echo off
rem ============================================================
rem  Visual check launcher (ASCII ONLY -- see build.bat for why)
rem
rem  Opens the widget for 15 seconds. Press Esc to close early.
rem
rem  What to look at (machine cannot judge these):
rem    1. Rounded corners, and OUTSIDE the corners you can see the
rem       desktop wallpaper (no dark square corners).
rem    2. No window border, no title bar.
rem    3. A white square slides left to right inside the panel,
rem       then wraps around -- that is the frame loop running.
rem    4. Try to click the widget: the window you were using before
rem       must NOT lose its highlight (the widget must not steal focus).
rem    5. Check the taskbar: the widget must NOT appear there.
rem    6. Press Alt+Tab: the widget must NOT be in the list.
rem
rem  Any failure keeps this window open so the message can be read.
rem  (A double-clicked .bat that exits instantly is the worst outcome:
rem   you cannot tell "wrong path" from "app crashed".)
rem ============================================================
setlocal
set "ROOT=%~dp0"
set "EXE=%ROOT%build\dshb.exe"

if not exist "%EXE%" (
  echo.
  echo [run] NOT FOUND: %EXE%
  echo [run] Build first by double-clicking build.bat, then try again.
  echo.
  pause
  exit /b 2
)

echo The widget appears for 15 seconds. Esc closes it early.
echo.
echo   - rounded translucent panel, no border, no title bar
echo   - white square sliding inside it
echo   - clicking it must not steal focus; not in taskbar; not in Alt+Tab
echo.
timeout /t 2 /nobreak >nul

"%EXE%" --seconds=15
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
  echo [run] widget exited normally.
) else (
  echo [run] widget exited with code %RC%
)
echo [run] diagnostics if anything looked wrong: %ROOT%build\selftest.log
echo.
pause

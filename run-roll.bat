@echo off
rem ============================================================
rem  Roll viewer + size chooser. ASCII ONLY.
rem
rem  Shows the widget and repeats the balance jump every 2 seconds
rem  so the digit rolling can be judged by eye.
rem
rem  THE SIZE QUESTION:
rem    at 100% the panel is 315 x 129 DIP, which is the design size.
rem    this screen runs at 200%, so that becomes 630 x 258 physical
rem    pixels -- twice the pixels, same physical size as the design.
rem
rem    if that still looks too big, this launcher lets you pick:
rem      1 = 1.00  panel 630 x 258 physical  (design intent)
rem      2 = 0.75  panel 473 x 194 physical
rem      3 = 0.60  panel 378 x 155 physical
rem      4 = 0.50  panel 315 x 129 physical  (1:1 with the design number)
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

echo Choose the visual size:
echo   1 = 1.00   panel 630 x 258 physical pixels  (design intent)
echo   2 = 0.75   panel 473 x 194 physical pixels
echo   3 = 0.60   panel 378 x 155 physical pixels
echo   4 = 0.50   panel 315 x 129 physical pixels  (1:1 with design)
echo   Enter = 1.00
echo.
set /p CHOICE=Your choice [1-4]:

set "SCALE=1.00"
if "%CHOICE%"=="2" set "SCALE=0.75"
if "%CHOICE%"=="3" set "SCALE=0.60"
if "%CHOICE%"=="4" set "SCALE=0.50"

echo.
echo Running at ui-scale=%SCALE%. Balance alternates 20.00 and 99.50.
echo Close this window to stop.
set DEEPSEEK_API_KEY=dummy-for-roll-view
timeout /t 2 /nobreak >nul
"%EXE%" --roll=loop --ui-scale=%SCALE%
echo [roll] exited
pause
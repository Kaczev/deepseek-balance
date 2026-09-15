@echo off
rem ============================================================
rem  deepseek-balance build entry point
rem
rem  ASCII ONLY. Never put non-ASCII text in this file: cmd.exe
rem  parses .bat with the OEM code page, so UTF-8 Chinese comments get
rem  shredded into stray commands. Chinese explanations live in the
rem  steps document under the "bu-rukku" (not-committed) folder.
rem
rem  Rules enforced here:
rem   1. vcvars64.bat must run first; cl/cmake/ninja are NOT on PATH.
rem   2. After vcvars64.bat, NEVER touch PATH.
rem   3. Build log goes to build\build.log, not stdout.
rem   4. EVERY source and header is checked against build\dshb.exe. If any is
rem      newer, the object cache is thrown away and everything is recompiled.
rem      WHY (measured 2026-09-15): this project's generated build.ninja has NO
rem      MSVC header dependency tracking -- no "deps = msvc" and no depfile --
rem      so ninja only compares the .cpp timestamps. Editing src\tuning.h (the
rem      file the owner tunes) therefore changed NOTHING: ninja printed
rem      "ninja: no work to do", the old exe was left in place, and the widget
rem      kept showing the old sizes. A full recompile costs a few seconds, and
rem      silently not rebuilding costs a lot more than that.
rem ============================================================
setlocal

set "VS_DIR=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "VCVARS=%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "LOG=build\build.log"

if not exist "build" mkdir build

if not exist "%VCVARS%" (
  echo [build] missing vcvars64.bat: "%VCVARS%" > "%LOG%"
  exit /b 2
)
if not exist "%CMAKE%" (
  echo [build] missing cmake: "%CMAKE%" > "%LOG%"
  exit /b 2
)

echo ==== %DATE% %TIME% ==== > "%LOG%"

rem ------------------------------------------------------------------
rem Is any tracked source newer than the existing artifact?
rem
rem Done with PowerShell because forfiles only accepts a DATE, not a
rem time-of-day -- "forfiles /D +09/15/2026" counts every file from today
rem as newer, and comparing against a date derived from the exe's own date
rem says "not newer" for changes made later the same day. Measured.
rem ------------------------------------------------------------------
set "STALE="
if exist "build\dshb.exe" (
  powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$exe=(Get-Item 'build\dshb.exe').LastWriteTime;" ^
    "$new=Get-ChildItem -Path src,tools -Recurse -Include *.h,*.cpp -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $exe };" ^
    "$cm=Get-Item 'CMakeLists.txt' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $exe };" ^
    "if($new -or $cm){ exit 1 } else { exit 0 }" >nul 2>&1
  if errorlevel 1 set "STALE=1"
)

rem A first-time build, or a stale one, gets the cache wiped so that every
rem object is genuinely recompiled rather than trusted.
set "RECONFIG="
if not exist "build\CMakeCache.txt" set "RECONFIG=1"
if defined STALE set "RECONFIG=1"

if defined RECONFIG (
  if defined STALE echo [build] a source is newer than the exe: forcing a full rebuild >> "%LOG%"
  echo [build] configuring ... >> "%LOG%"
  if exist "build\CMakeFiles" rd /s /q "build\CMakeFiles" >nul 2>&1
  if exist "build\CMakeCache.txt" del /q "build\CMakeCache.txt" >nul 2>&1
  call "%VCVARS%" >nul 2>&1
  if errorlevel 1 exit /b 3
  "%CMAKE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="%NINJA%" >> "%LOG%" 2>&1
  if errorlevel 1 exit /b 4
)

echo [build] compiling ... >> "%LOG%"
call "%VCVARS%" >nul 2>&1
if errorlevel 1 exit /b 3
"%CMAKE%" --build build >> "%LOG%" 2>&1
set "BUILD_RC=%ERRORLEVEL%"
if not "%BUILD_RC%"=="0" (
  echo [build] FAILED rc=%BUILD_RC% -- see the lines above in this log >> "%LOG%"
  exit /b 5
)

if exist "build\dshb.exe" (
  echo [build] OK build\dshb.exe >> "%LOG%"
  exit /b 0
) else (
  echo [build] build reported success but no artifact found >> "%LOG%"
  exit /b 6
)

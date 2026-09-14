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
rem   2. After vcvars64.bat, NEVER touch PATH. Doing so replaces PATH
rem      with an expanded copy, silently drops the MSVC toolchain, and
rem      cmake then reports "No CMAKE_CXX_COMPILER could be found" --
rem      which looks like a missing compiler but is not.
rem   3. Build log goes to build\build.log, not stdout, so that a pipe
rem      on the caller side cannot stall the build.
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

rem CMakeCache.txt is the authoritative marker that the build dir is configured.
rem Checking build.ninja instead breaks as soon as the cache is deleted alone:
rem the script then skips configuring and cmake reports "not a CMake build
rem directory (missing CMakeCache.txt)".
if not exist "build\CMakeCache.txt" (
  echo [build] configuring ... >> "%LOG%"
  call "%VCVARS%" >nul 2>&1
  if errorlevel 1 exit /b 3
  "%CMAKE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="%NINJA%" >> "%LOG%" 2>&1
  if errorlevel 1 exit /b 4
)

echo [build] compiling ... >> "%LOG%"
call "%VCVARS%" >nul 2>&1
if errorlevel 1 exit /b 3
"%CMAKE%" --build build >> "%LOG%" 2>&1
if errorlevel 1 exit /b 5

if exist "build\dshb.exe" (
  echo [build] OK build\dshb.exe >> "%LOG%"
  exit /b 0
) else (
  echo [build] build succeeded but no artifact found >> "%LOG%"
  exit /b 6
)

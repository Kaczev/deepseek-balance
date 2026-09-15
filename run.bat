@echo off
rem Run the widget against the REAL DeepSeek API.
rem
rem Why this file exists: run-roll.bat is a demo script for the roll animation. It
rem takes the export/demo path, which pre-warms a fake balance (20.00 <-> 99.50) and
rem returns before the real sampling source is ever started - so it always shows test
rem numbers. This script runs the normal path instead.
cd /d "%~dp0build"
"%~dp0build\dshb.exe" %*

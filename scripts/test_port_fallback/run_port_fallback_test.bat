@echo off
REM Port 13619 Occupancy Test - Launcher
REM Double-click this file to run, or run from terminal

set "SCRIPT_DIR=%~dp0"
powershell -ExecutionPolicy Bypass -File "%SCRIPT_DIR%test_port_fallback_win.ps1" %*
pause

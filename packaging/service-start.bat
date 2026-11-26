@echo off
REM =====================================================================
REM  Start the installed Desk2ONVIF Windows service.
REM
REM  This is for SERVICE mode only. A console-mode copy has no service and
REM  no watchdog: close its window or press Ctrl+C to stop it.
REM
REM  The exe self-elevates (UAC) when it is not running as administrator.
REM  ASCII-only, same reason as start.bat.
REM =====================================================================
chcp 65001 >nul
echo.
"%~dp0desk2onvif.exe" --service start
echo.
pause

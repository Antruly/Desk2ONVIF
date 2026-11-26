@echo off
REM =====================================================================
REM  Stop the installed Desk2ONVIF Windows service.
REM
REM  This is the correct way to stop service mode. Do NOT kill the worker
REM  in Task Manager: the Session 0 watchdog treats that as a crash and
REM  starts a new worker after a short backoff.
REM
REM  This stops the service but keeps it installed, so it will start again
REM  on the next boot. To remove it completely use the uninstaller.
REM
REM  The exe self-elevates (UAC) when it is not running as administrator.
REM  ASCII-only, same reason as start.bat.
REM =====================================================================
chcp 65001 >nul
echo.
"%~dp0desk2onvif.exe" --service stop
echo.
pause

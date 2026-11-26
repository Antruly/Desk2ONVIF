@echo off
REM =====================================================================
REM  Show the state of the Desk2ONVIF Windows service and keep the window
REM  open so the result can be read. ASCII-only, same reason as start.bat.
REM =====================================================================
chcp 65001 >nul
echo.
"%~dp0desk2onvif.exe" --service status
echo.
echo --- sc query Desk2ONVIF ---
sc query Desk2ONVIF
echo.
echo Stop it with: service-stop.bat  (kill worker in Task Manager -> watchdog restarts it)
echo.
pause

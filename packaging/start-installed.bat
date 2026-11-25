@echo off
REM =====================================================================
REM  Desk2ONVIF launcher for a normal (console-mode) install.
REM
REM  Unlike start.bat (portable zip), this one reads the machine-wide
REM  config the installer wrote:
REM
REM      %ProgramData%\Desk2ONVIF\config.json
REM
REM  The config carries the ports, listen address and credentials chosen
REM  in the setup wizard, so start.ps1 is told to use it verbatim. Any
REM  extra arguments are still passed through:
REM
REM      start-installed.bat -RtspPort 8554
REM
REM  This file is deliberately ASCII-only, for the same reason as
REM  start.bat: a UTF-8 .bat is mis-parsed by cmd.exe under codepage 936.
REM =====================================================================
chcp 65001 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -Config "%ProgramData%\Desk2ONVIF\config.json" %*
echo.
pause

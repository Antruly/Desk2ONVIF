@echo off
REM =====================================================================
REM  Desk2ONVIF -- local-only mode.
REM
REM  Web console listens on 127.0.0.1, so nothing on the LAN can reach it.
REM  Use this when you only want to preview/configure on this machine and
REM  do NOT want every machine on the network able to rewrite your config
REM  (the web console has no authentication).
REM
REM  Note: an NVR on another machine will NOT be able to reach this. For
REM  NVR use, run start.bat instead and mind the firewall.
REM =====================================================================
chcp 65001 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -LocalOnly %*
echo.
pause

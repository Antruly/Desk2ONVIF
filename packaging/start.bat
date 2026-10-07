@echo off
REM =====================================================================
REM  Desk2ONVIF launcher (default: RTSP 554 + Web 8080 + ONVIF on)
REM
REM  This file is deliberately ASCII-only. A .bat whose text is UTF-8
REM  (with or without a BOM) gets mis-parsed by cmd.exe under the usual
REM  936 codepage -- quotes inside Chinese comments pair up wrongly and
REM  you get a syntax error that points nowhere near the cause. All the
REM  Chinese help text lives in start.ps1, which PowerShell reads
REM  correctly because it honours the BOM.
REM
REM  Extra arguments are passed straight through:
REM      start.bat --fps 60
REM =====================================================================
chcp 65001 >nul
REM  -Portable: config.json lives next to the exe. The installer uses a
REM  per-user install dir (PrivilegesRequired=lowest -> %LOCALAPPDATA%\Programs),
REM  which is writable, so this works identically for installed and unzipped.
REM  Drop -Portable if you'd rather have %APPDATA%\Desk2ONVIF\config.json.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -Portable %*
echo.
pause

@echo off
REM =====================================================================
REM  Desk2ONVIF launcher -- local-only mode.
REM
REM  Same as start.bat, except the web console binds 127.0.0.1 instead of
REM  0.0.0.0, so nothing on the LAN can reach it.
REM
REM  This is NOT the same thing as "no authentication" (an earlier version
REM  of this comment said that, and it stopped being true when the console
REM  grew a login). The console still asks for a username and password, and
REM  ONVIF still challenges with Digest. What changes here is reachability:
REM  on 0.0.0.0 with the factory password still in place, any machine on the
REM  network can log in, watch this desktop, and rewrite the config; on
REM  127.0.0.1 only this machine can. Change the password anyway -- it is
REM  one form field on the config page.
REM
REM  An NVR on another machine will NOT be able to reach this. For NVR use,
REM  run start.bat instead and mind the firewall.
REM
REM  Config location depends on which flavour this copy is:
REM    * portable zip  -> config.json sits next to the exe; use -Portable
REM    * installed     -> the installer also ships start-installed.bat and writes
REM                       %ProgramData%\Desk2ONVIF\config.json; use -Config
REM  The presence of start-installed.bat is the marker: only the installer
REM  puts it here (the portable zip deliberately excludes it). Using
REM  -Portable on an installed copy would read {app}\config.json, which no
REM  longer exists, so the username/password typed during setup would
REM  silently fall back to the built-in defaults.
REM
REM  This file is deliberately ASCII-only, for the same reason start.bat is:
REM  cmd.exe under codepage 936 mis-pairs the quotes in non-ASCII comments
REM  and reports a syntax error nowhere near the cause.
REM
REM  Extra arguments are passed straight through:
REM      start-local.bat --fps 60
REM =====================================================================
chcp 65001 >nul
if exist "%~dp0start-installed.bat" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -LocalOnly -Config "%ProgramData%\Desk2ONVIF\config.json" %*
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -LocalOnly -Portable %*
)
echo.
pause

@echo off
REM =====================================================================
REM  Dependency self-check: verifies FFmpeg / Live555 / libuvcpp are
REM  present, loadable, and actually linked (not just headers found).
REM
REM  If start.bat fails before printing any URL, run this: it separates
REM  "a DLL is missing" from "a port is taken" from "no capturable
REM  display", which otherwise all look like "it didn't start".
REM =====================================================================
chcp 65001 >nul
pushd "%~dp0"
desk2onvif.exe --selfcheck
popd
echo.
pause

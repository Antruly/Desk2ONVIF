@echo off
REM =====================================================================
REM  Print the firewall commands needed for an NVR on another machine.
REM
REM  Read-only: this does NOT touch your firewall. It only prints the
REM  netsh lines. Run the printed command yourself in an ADMIN prompt if
REM  you agree with it -- a screen recorder silently opening ports to the
REM  LAN is not something that should happen without you asking.
REM
REM  Desk2ONVIF never modifies the firewall on its own, not even when you
REM  start the RTSP or web server.
REM
REM  Symptom that points here: ffplay on THIS machine plays fine, but an
REM  NVR or another machine cannot connect or discover the device. Local
REM  playback goes over the loopback interface and is never filtered, so
REM  "works locally, unreachable from elsewhere" is the classic signature.
REM =====================================================================
chcp 65001 >nul
pushd "%~dp0"
desk2onvif.exe --firewall print --rtsp --http
popd
echo.
pause

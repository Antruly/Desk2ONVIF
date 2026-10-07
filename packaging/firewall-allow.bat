@echo off
REM =====================================================================
REM  Open the inbound firewall rules Desk2ONVIF needs (asks for UAC).
REM
REM  Only needed when an NVR or another machine must reach this one.
REM  Runs on THIS machine only, and only for profiles private/domain and
REM  remoteip=localsubnet -- not open to the whole internet, and not open
REM  on public networks (cafe/hotel wifi).
REM
REM  Desk2ONVIF itself NEVER touches the firewall, not even when you start
REM  the RTSP or web server. This shortcut exists so the change is
REM  something you explicitly ask for, and so you can see the UAC prompt.
REM
REM  To see the commands without running them, use firewall-print.bat.
REM =====================================================================
chcp 65001 >nul
cd /d "%~dp0"

echo Requesting administrator rights...
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$exe = Join-Path $PWD 'desk2onvif.exe';" ^
  "$cfg = Join-Path $PWD 'config.json';" ^
  "$a = @('--firewall','add','--rtsp','--http');" ^
  "if (Test-Path -LiteralPath $cfg) { $a += @('--config', $cfg) };" ^
  "Start-Process -FilePath $exe -ArgumentList $a -Verb RunAs -Wait"

echo.
pause

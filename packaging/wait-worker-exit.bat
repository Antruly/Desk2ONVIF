@echo off
REM =====================================================================
REM  Wait (bounded) for the Session-1 worker to finish after
REM  "desk2onvif.exe --service uninstall".
REM
REM  Why this exists: the uninstaller deletes {app} right after
REM  [UninstallRun]. "desk2onvif.exe --service uninstall" waits for the
REM  SCM service to stop, but the worker it launched into the logged-in
REM  user's session finishes a moment later. If the uninstaller runs while
REM  it is still alive, Windows refuses to delete the running exe and its
REM  open log file, leaving a half-removed install directory.
REM
REM  Bounded at 30 seconds: a console-mode instance the user started by
REM  hand is not ours to kill, and a timeout is better than hanging the
REM  uninstaller forever. ASCII-only, same reason as start.bat.
REM =====================================================================
setlocal
for /L %%i in (1,1,30) do (
    tasklist /FI "IMAGENAME eq desk2onvif.exe" /NH 2>nul | find /I "desk2onvif.exe" >nul
    if errorlevel 1 goto done
    ping -n 2 127.0.0.1 >nul
)
:done
exit /b 0

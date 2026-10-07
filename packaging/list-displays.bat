@echo off
REM =====================================================================
REM  List display outputs and probe whether each one can actually be
REM  captured by DXGI Desktop Duplication.
REM
REM  Run this FIRST. Two reasons:
REM
REM   1. It tells you the index to pass to --display, so you can record a
REM      specific monitor instead of guessing.
REM   2. It answers "can this machine be recorded at all". Virtual display
REM      drivers (Sunlogin/Oray IDD, some remote-desktop and virtual-display
REM      adapters) do not support Desktop Duplication. The symptom without
REM      this check is a black or frozen picture and no error anywhere --
REM      because DuplicateOutput succeeds and simply never delivers frames.
REM
REM  Guessing from the adapter name does not work: it both misses unknown
REM  vendors and false-positives on machine names that merely contain
REM  "virtual". This probe really creates a DuplicateOutput per output.
REM =====================================================================
chcp 65001 >nul
pushd "%~dp0"
desk2onvif.exe --list-displays
popd
echo.
pause

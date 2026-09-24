@echo off
rem Convenience wrapper so Build-Plugins.ps1 can be double-clicked or run from
rem cmd. Any arguments are forwarded, e.g. Build-Plugins.cmd -Target server
setlocal
set "PS=pwsh.exe"
where /q pwsh.exe || set "PS=powershell.exe"
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Plugins.ps1" -Parallel 8 %*
echo.
pause

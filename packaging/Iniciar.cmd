@echo off
setlocal
cd /d "%~dp0"
if not exist "%~dp0Iniciar.ps1" (
  echo Nao foi possivel localizar Iniciar.ps1. Extraia todo o ZIP antes de iniciar.
  pause
  exit /b 1
)
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Iniciar.ps1"
set "launcher_result=%errorlevel%"
if not "%launcher_result%"=="0" pause
exit /b %launcher_result%

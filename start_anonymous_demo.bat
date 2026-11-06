@echo off
cd /d "%~dp0"
if not exist "build\ipc_messenger.exe" (
  echo Please build the project first. Run build_mingw.bat or build_msvc.bat.
  pause
  exit /b 1
)
"build\ipc_messenger.exe" --server anonymous --clients 2

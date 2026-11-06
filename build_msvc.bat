@echo off
setlocal
cd /d "%~dp0"
where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: CMake was not found.
  pause
  exit /b 1
)
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :fail
cmake --build build-msvc --config Release
if errorlevel 1 goto :fail
echo.
echo Build successful: build-msvc\Release\ipc_messenger.exe
pause
exit /b 0
:fail
echo Build failed. Make sure Visual Studio 2022 C++ Build Tools are installed.
pause
exit /b 1

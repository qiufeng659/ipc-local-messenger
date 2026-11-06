@echo off
setlocal
cd /d "%~dp0"
where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: CMake was not found. Install CMake and add it to PATH.
  pause
  exit /b 1
)
where g++ >nul 2>nul
if errorlevel 1 (
  echo ERROR: g++ was not found. Install a native Windows MinGW-w64 toolchain, e.g. MSYS2 UCRT64.
  pause
  exit /b 1
)
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :fail
cmake --build build --config Release
if errorlevel 1 goto :fail
echo.
echo Build successful: build\ipc_messenger.exe
pause
exit /b 0
:fail
echo.
echo Build failed. Read the first compiler error above.
pause
exit /b 1

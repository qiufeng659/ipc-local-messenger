@echo off
setlocal EnableExtensions

rem ============================================================
rem  Local Messenger IPC launcher
rem  Usage:
rem     run.bat               -> interactive menu
rem     run.bat anonymous     -> anonymous pipes
rem     run.bat named         -> named pipes
rem     run.bat mailslot      -> mailslot
rem     run.bat socket        -> local socket
rem ============================================================

cd /d "%~dp0"

set "EXE="
if exist "build\ipc_messenger.exe" set "EXE=build\ipc_messenger.exe"
if not defined EXE if exist "build-msvc\Release\ipc_messenger.exe" set "EXE=build-msvc\Release\ipc_messenger.exe"

if not defined EXE (
    echo.
    echo [ERROR] ipc_messenger.exe was not found.
    echo         Please run build_mingw.bat or build_msvc.bat first.
    echo.
    pause
    exit /b 1
)

if /i "%~1"=="anonymous" goto RUN_ANON
if /i "%~1"=="named"     goto RUN_NAMED
if /i "%~1"=="mailslot"  goto RUN_MAILSLOT
if /i "%~1"=="socket"    goto RUN_SOCKET

:MENU
cls
echo ============================================================
echo   Local Messenger  -  Windows IPC demo launcher
echo ============================================================
echo   Program : %EXE%
echo.
echo   1) Anonymous Pipe    (server auto-spawns 2 child clients)
echo   2) Named Pipe        (auto: server + Alice + Bob)
echo   3) Mailslot          (auto: server + Alice + Bob)
echo   4) Local Socket      (auto: server + Alice + Bob)
echo   0) Exit
echo ============================================================
set "CHOICE="
set /p "CHOICE=Select mode: "
if not defined CHOICE goto MENU

if "%CHOICE%"=="1" goto RUN_ANON
if "%CHOICE%"=="2" goto RUN_NAMED
if "%CHOICE%"=="3" goto RUN_MAILSLOT
if "%CHOICE%"=="4" goto RUN_SOCKET
if "%CHOICE%"=="0" exit /b 0
goto MENU

:RUN_ANON
cls
echo Starting anonymous-pipe mode.
echo The server will spawn two child client consoles automatically.
echo Type /quit in the server window to stop the demo.
echo.
"%EXE%" --server anonymous --clients 2
goto AFTER_RUN

:RUN_NAMED
cls
echo Starting named-pipe server and 2 clients...
start "IPC Server  - Named Pipe" cmd /k ""%EXE%" --server named-pipe"
timeout /t 2 /nobreak >nul
start "IPC Client Alice - Named Pipe" cmd /k ""%EXE%" --client named-pipe Alice"
timeout /t 1 /nobreak >nul
start "IPC Client Bob   - Named Pipe" cmd /k ""%EXE%" --client named-pipe Bob"
goto AFTER_RUN

:RUN_MAILSLOT
cls
echo Starting mailslot server and 2 clients...
start "IPC Server  - Mailslot" cmd /k ""%EXE%" --server mailslot"
timeout /t 2 /nobreak >nul
start "IPC Client Alice - Mailslot" cmd /k ""%EXE%" --client mailslot Alice"
timeout /t 1 /nobreak >nul
start "IPC Client Bob   - Mailslot" cmd /k ""%EXE%" --client mailslot Bob"
goto AFTER_RUN

:RUN_SOCKET
cls
echo Starting local-socket server and 2 clients...
start "IPC Server  - Local Socket" cmd /k ""%EXE%" --server socket"
timeout /t 2 /nobreak >nul
start "IPC Client Alice - Local Socket" cmd /k ""%EXE%" --client socket Alice"
timeout /t 1 /nobreak >nul
start "IPC Client Bob   - Local Socket" cmd /k ""%EXE%" --client socket Bob"
goto AFTER_RUN

:AFTER_RUN
echo.
echo Server and clients are running in their own windows.
echo Close those windows or type /quit inside them to end the session.
echo.
pause
goto MENU
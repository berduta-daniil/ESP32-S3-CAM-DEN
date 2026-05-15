@echo off
setlocal
cd /d "%~dp0"

where node >nul 2>nul
if errorlevel 1 (
  echo Node.js is not installed or is not in PATH.
  pause
  exit /b 1
)

if "%ESP32_AUTH_USER%"=="" set ESP32_AUTH_USER=cam
if "%ESP32_AUTH_PASSWORD%"=="" set ESP32_AUTH_PASSWORD=1234
if "%ESP32_HTTP_PORT%"=="" set ESP32_HTTP_PORT=80

node find-board.js
echo.
pause

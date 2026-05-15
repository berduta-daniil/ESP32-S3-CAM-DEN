@echo off
setlocal
cd /d "%~dp0"

where node >nul 2>nul
if errorlevel 1 (
  echo Node.js is not installed or is not in PATH.
  echo Install Node.js LTS from https://nodejs.org/ and run this file again.
  pause
  exit /b 1
)

if "%ESP32_HTTP_PORT%"=="" set ESP32_HTTP_PORT=80
if "%ESP32_AUDIO_PORT%"=="" set ESP32_AUDIO_PORT=81
if "%PROXY_PORT%"=="" set PROXY_PORT=8080

if "%ESP32_AUTH_USER%"=="" set ESP32_AUTH_USER=cam
if "%ESP32_AUTH_PASSWORD%"=="" set ESP32_AUTH_PASSWORD=1234

if "%ESP32_HOST%"=="" (
  echo Searching for ESP32 board in local networks...
  for /f "usebackq tokens=*" %%i in (`node find-board.js --first`) do set ESP32_HOST=%%i
)

if "%ESP32_HOST%"=="" (
  set /p ESP32_HOST=ESP32 IP address [192.168.4.1]: 
  if "%ESP32_HOST%"=="" set ESP32_HOST=192.168.4.1
)

echo.
echo ESP32 video: http://%ESP32_HOST%:%ESP32_HTTP_PORT%/stream
echo ESP32 audio: ws://%ESP32_HOST%:%ESP32_AUDIO_PORT%/audio.ws
echo PC proxy:    http://localhost:%PROXY_PORT%/
echo.

node find-board.js --check "%ESP32_HOST%"
if errorlevel 1 (
  echo.
  echo The PC cannot reach the ESP32 at %ESP32_HOST%.
  echo Check that the board and this PC are in the same local network.
  echo You can run find-board.bat to search again.
  echo.
  pause
  exit /b 1
)

if "%INTERNET_TUNNEL%"=="" (
  set /p INTERNET_TUNNEL=Open video to internet via Cloudflare Tunnel? [y/N]: 
)

if /I "%INTERNET_TUNNEL%"=="Y" goto internet
if /I "%INTERNET_TUNNEL%"=="YES" goto internet

set OPEN_BROWSER=1
node server.js
goto done

:internet
if "%PROXY_TOKEN%"=="" set PROXY_TOKEN=esp32-%RANDOM%-%RANDOM%-%RANDOM%

if not exist tools mkdir tools
set CLOUDFLARED=%~dp0tools\cloudflared.exe

if not exist "%CLOUDFLARED%" (
  echo.
  echo Downloading Cloudflare Tunnel client...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri 'https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe' -OutFile '%CLOUDFLARED%'"
  if errorlevel 1 (
    echo Failed to download cloudflared.exe.
    pause
    exit /b 1
  )
)

echo.
echo Local proxy:  http://localhost:%PROXY_PORT%/
echo Access token: %PROXY_TOKEN%
echo.
echo The script will print and open the real public trycloudflare.com URL.
echo Keep this window open while streaming.
echo Press Ctrl+C to stop the internet tunnel.
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-internet.ps1" -CloudflaredPath "%CLOUDFLARED%"

:done
echo.
pause

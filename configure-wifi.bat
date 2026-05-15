@echo off
setlocal
cd /d "%~dp0"

set /p WIFI_STA_SSID=Router Wi-Fi SSID: 
if "%WIFI_STA_SSID%"=="" (
  echo SSID is required.
  pause
  exit /b 1
)

set /p WIFI_STA_PASSWORD=Router Wi-Fi password: 

if not exist include mkdir include

(
  echo #pragma once
  echo.
  echo #define WIFI_STA_SSID "%WIFI_STA_SSID%"
  echo #define WIFI_STA_PASSWORD "%WIFI_STA_PASSWORD%"
  echo.
  echo #define WIFI_AP_SSID "ESP32-S3-CAM-DEN"
  echo #define WIFI_AP_PASSWORD "12345678"
  echo.
  echo #define HTTP_AUTH_USER "cam"
  echo #define HTTP_AUTH_PASSWORD "1234"
) > include\wifi_config.h

echo.
echo Created include\wifi_config.h
echo Now rebuild and upload:
echo   pio run -t upload
echo.
pause

#pragma once

// Copy to `include/mqtt_bridge_config.h`, set the same values on the GitHub Pages
// site, rebuild and upload.

#define MQTT_BRIDGE_ENABLED 1

// Official public EMQX broker:
// - MQTT over TCP: broker.emqx.io:1883
// - MQTT over Secure WebSocket: wss://broker.emqx.io:8084/mqtt
#define MQTT_BROKER_HOST "broker.emqx.io"
#define MQTT_BROKER_PORT 1883
#define MQTT_BROKER_WSS_URL "wss://broker.emqx.io:8084/mqtt"
#define MQTT_USE_TLS 0

// For maximum compatibility on ESP32 the board uses plain MQTT TCP.
// The browser still uses secure WSS on GitHub Pages.
#define MQTT_ALLOW_INSECURE_TLS 1

// Use a long random secret because this broker is public.
// Everyone on the public broker can theoretically observe messages.
#define MQTT_DEVICE_ID "esp32-s3-cam-den-01"
#define MQTT_SHARED_KEY "replace-with-very-long-random-shared-key"

// Cloud stream tuning.
#define MQTT_VIDEO_INTERVAL_MS 450
#define MQTT_STATE_INTERVAL_MS 10000
#define MQTT_RECONNECT_INTERVAL_MS 5000

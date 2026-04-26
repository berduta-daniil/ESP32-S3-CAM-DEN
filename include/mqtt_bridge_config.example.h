#pragma once

// Copy to `include/mqtt_bridge_config.h`, set the same values on the GitHub Pages
// site, rebuild and upload.

#define MQTT_BRIDGE_ENABLED 1

// Official public EMQX broker:
// - MQTT over TLS: broker.emqx.io:8883
// - MQTT over Secure WebSocket: wss://broker.emqx.io:8084/mqtt
#define MQTT_BROKER_HOST "broker.emqx.io"
#define MQTT_BROKER_PORT 8883
#define MQTT_BROKER_WSS_URL "wss://broker.emqx.io:8084/mqtt"

// For maximum compatibility on ESP32 we use TLS without CA pinning.
#define MQTT_ALLOW_INSECURE_TLS 1

// Use a long random secret because this broker is public.
// Everyone on the public broker can theoretically observe messages.
#define MQTT_DEVICE_ID "esp32-s3-cam-den-01"
#define MQTT_SHARED_KEY "replace-with-very-long-random-shared-key"

// Cloud stream tuning.
#define MQTT_VIDEO_INTERVAL_MS 450
#define MQTT_STATE_INTERVAL_MS 10000
#define MQTT_RECONNECT_INTERVAL_MS 5000

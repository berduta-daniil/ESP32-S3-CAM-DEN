#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>
#include "driver/i2s.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"
#include "freertos/semphr.h"
#include "img_converters.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#include "audio_pins.h"
#include "camera_pins.h"
#include "speaker_pins.h"

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#endif

#if __has_include("relay_config.h")
#include "relay_config.h"
#endif

#if __has_include("mqtt_bridge_config.h")
#include "mqtt_bridge_config.h"
#endif

#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID "TP-Link_7B6A"
#endif

#ifndef WIFI_STA_PASSWORD
#define WIFI_STA_PASSWORD "28180969"
#endif

#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID "ESP32-S3-CAM-DEN"
#endif

#ifndef WIFI_AP_PASSWORD
#define WIFI_AP_PASSWORD "12345678"
#endif

#ifndef HTTP_AUTH_USER
#define HTTP_AUTH_USER "cam"
#endif

#ifndef HTTP_AUTH_PASSWORD
#define HTTP_AUTH_PASSWORD "1234"
#endif

#ifndef RELAY_ENABLED
#define RELAY_ENABLED 0
#endif

#ifndef RELAY_HOST
#define RELAY_HOST ""
#endif

#ifndef RELAY_PORT
#define RELAY_PORT 443
#endif

#ifndef RELAY_USE_TLS
#define RELAY_USE_TLS 1
#endif

#ifndef RELAY_ALLOW_INSECURE_TLS
#define RELAY_ALLOW_INSECURE_TLS 0
#endif

#ifndef RELAY_BASE_PATH
#define RELAY_BASE_PATH ""
#endif

#ifndef RELAY_DEVICE_ID
#define RELAY_DEVICE_ID ""
#endif

#ifndef RELAY_DEVICE_TOKEN
#define RELAY_DEVICE_TOKEN ""
#endif

#ifndef RELAY_FRAME_INTERVAL_MS
#define RELAY_FRAME_INTERVAL_MS 160
#endif

#ifndef RELAY_STATE_INTERVAL_MS
#define RELAY_STATE_INTERVAL_MS 10000
#endif

#ifndef RELAY_RECONNECT_INTERVAL_MS
#define RELAY_RECONNECT_INTERVAL_MS 5000
#endif

#ifndef RELAY_INTERCOM_ENABLED
#define RELAY_INTERCOM_ENABLED 0
#endif

#ifndef MQTT_BRIDGE_ENABLED
#define MQTT_BRIDGE_ENABLED 0
#endif

#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "broker.emqx.io"
#endif

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883
#endif

#ifndef MQTT_BROKER_WSS_URL
#define MQTT_BROKER_WSS_URL "wss://broker.emqx.io:8084/mqtt"
#endif

#ifndef MQTT_USE_TLS
#define MQTT_USE_TLS 0
#endif

#ifndef MQTT_ALLOW_INSECURE_TLS
#define MQTT_ALLOW_INSECURE_TLS 1
#endif

#ifndef MQTT_DEVICE_ID
#define MQTT_DEVICE_ID ""
#endif

#ifndef MQTT_SHARED_KEY
#define MQTT_SHARED_KEY ""
#endif

#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif

#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif

#ifndef MQTT_VIDEO_INTERVAL_MS
#define MQTT_VIDEO_INTERVAL_MS 450
#endif

#ifndef MQTT_STATE_INTERVAL_MS
#define MQTT_STATE_INTERVAL_MS 10000
#endif

#ifndef MQTT_RECONNECT_INTERVAL_MS
#define MQTT_RECONNECT_INTERVAL_MS 5000
#endif

constexpr uint16_t HTTP_PORT = 80;
constexpr uint16_t AUDIO_PORT = 81;
constexpr uint16_t SPEAKER_INPUT_PORT = 82;
constexpr uint32_t AUDIO_SERVER_TASK_STACK = 16384;
constexpr uint32_t SPEAKER_INPUT_TASK_STACK = 12288;
constexpr bool ENABLE_DIRECT_AP = true;
constexpr bool INIT_CAMERA_BEFORE_WIFI = false;
constexpr bool KEEP_CAMERA_INITIALIZED = false;
constexpr bool AUTO_START_AV_ON_PAGE_LOAD = true;
constexpr framesize_t CAMERA_FRAME_SIZE = FRAMESIZE_QVGA;
constexpr int CAMERA_JPEG_QUALITY = 14;
constexpr framesize_t RELAY_AV_CAMERA_FRAME_SIZE = FRAMESIZE_CIF;
constexpr int RELAY_AV_CAMERA_JPEG_QUALITY = 12;
constexpr framesize_t RELAY_VIDEO_ONLY_CAMERA_FRAME_SIZE = FRAMESIZE_HVGA;
constexpr int RELAY_VIDEO_ONLY_CAMERA_JPEG_QUALITY = 18;
constexpr framesize_t MQTT_AV_CAMERA_FRAME_SIZE = FRAMESIZE_QQVGA;
constexpr int MQTT_AV_CAMERA_JPEG_QUALITY = 24;
constexpr framesize_t MQTT_VIDEO_ONLY_CAMERA_FRAME_SIZE = FRAMESIZE_QVGA;
constexpr int MQTT_VIDEO_ONLY_CAMERA_JPEG_QUALITY = 18;
constexpr size_t CAMERA_FB_COUNT = 1;
constexpr uint16_t STREAM_DELAY_MS = 80;
constexpr uint32_t CAMERA_IDLE_STOP_MS = 30000;
constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr size_t AUDIO_FRAMES = 256;
constexpr i2s_port_t MIC_I2S_PORT = I2S_NUM_0;
constexpr int AUDIO_SAMPLE_SHIFT = 0;
constexpr int AUDIO_SAMPLE_GAIN = 4;
constexpr int RELAY_INTERCOM_CAPTURE_GAIN = 8;
constexpr uint32_t SPEAKER_SAMPLE_RATE = 16000;
constexpr size_t SPEAKER_FRAMES = 256;
constexpr i2s_port_t SPEAKER_I2S_PORT = I2S_NUM_1;
constexpr int SPEAKER_DEFAULT_AMPLITUDE = 2500;
constexpr int RELAY_INTERCOM_PLAYBACK_GAIN = 2;
constexpr size_t MQTT_AUDIO_FRAMES = 512;
constexpr uint8_t RELAY_PACKET_VIDEO_JPEG = 1;
constexpr uint8_t RELAY_PACKET_AUDIO_PCM = 2;

WebServer server(HTTP_PORT);
WiFiServer audioServer(AUDIO_PORT);
WiFiServer speakerInputServer(SPEAKER_INPUT_PORT);
WebSocketsClient relayControlSocket;
WebSocketsClient relayMediaSocket;
WebSocketsClient relaySpeakerSocket;
#if MQTT_USE_TLS
WiFiClientSecure mqttTransport;
#else
WiFiClient mqttTransport;
#endif
PubSubClient mqttClient(mqttTransport);

SemaphoreHandle_t speakerUseMutex = nullptr;
SemaphoreHandle_t relaySocketMutex = nullptr;
SemaphoreHandle_t mqttClientMutex = nullptr;
TaskHandle_t relayMediaTaskHandle = nullptr;
TaskHandle_t mqttMediaTaskHandle = nullptr;

bool cameraReady = false;
bool microphoneReady = false;
bool speakerReady = false;
String cameraStatus = "standby";
String microphoneStatus = "standby";
String speakerStatus = "standby";
esp_err_t lastMicrophoneReadError = ESP_OK;
size_t lastMicrophoneBytesRead = 0;
unsigned long lastCameraUseMs = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long lastRelayStateMs = 0;
unsigned long lastRelaySpeakerChunkMs = 0;
unsigned long lastMqttStateMs = 0;
unsigned long lastMqttConnectAttemptMs = 0;
unsigned long lastMqttSpeakerChunkMs = 0;

bool relayControlConnected = false;
bool relayMediaConnected = false;
bool relaySpeakerConnected = false;
bool relayVideoRequested = false;
bool relayAudioRequested = false;
bool relayUseRightChannel = false;
int relayAudioShift = AUDIO_SAMPLE_SHIFT;
int relayAudioGain = AUDIO_SAMPLE_GAIN;
String relayStatus = RELAY_ENABLED ? "configured" : "disabled";
String relayControlPath;
String relayMediaPath;
String relaySpeakerPath;
bool mqttConnected = false;
bool mqttVideoRequested = false;
bool mqttAudioRequested = false;
bool mqttUseRightChannel = false;
int mqttAudioShift = AUDIO_SAMPLE_SHIFT;
int mqttAudioGain = AUDIO_SAMPLE_GAIN;
String mqttStatus = MQTT_BRIDGE_ENABLED ? "configured" : "disabled";
String mqttTopicPrefix;
String mqttControlTopic;
String mqttVideoTopic;
String mqttAudioTopic;
String mqttSpeakerTopic;
String mqttStateTopic;
String mqttPresenceTopic;
framesize_t activeCameraFrameSize = CAMERA_FRAME_SIZE;
int activeCameraJpegQuality = CAMERA_JPEG_QUALITY;

static void stopCamera();
static void relayControlSocketEvent(WStype_t type, uint8_t *payload, size_t length);
static void relayMediaSocketEvent(WStype_t type, uint8_t *payload, size_t length);
static void relaySpeakerSocketEvent(WStype_t type, uint8_t *payload, size_t length);
static void relayMediaTask(void *parameter);
static void relaySendStateLine();
static void startRelayClients();
static void mqttCallback(char *topic, uint8_t *payload, unsigned int length);
static void mqttMediaTask(void *parameter);
static bool connectMqttBridge();
static void startMqttBridge();
static void mqttSendStateLine();
static void applyCameraSensorProfile(framesize_t frameSize, int jpegQuality, bool fullTuning = false);
static void markMqttDisconnected(const char *status);
static bool mqttTalkbackActive();
static bool mqttHasBrokerCredentials();
static bool relayTalkbackActive();
static bool hybridCloudMode();
static bool relayIntercomEnabled();

static bool relayConfigured() {
  return RELAY_ENABLED &&
         strlen(RELAY_HOST) > 0 &&
         strlen(RELAY_DEVICE_ID) > 0 &&
         strlen(RELAY_DEVICE_TOKEN) > 0;
}

static bool mqttBridgeConfigured() {
  String sharedKey = MQTT_SHARED_KEY;
  return MQTT_BRIDGE_ENABLED &&
         strlen(MQTT_BROKER_HOST) > 0 &&
         strlen(MQTT_DEVICE_ID) > 0 &&
         sharedKey.length() >= 16 &&
         sharedKey != "replace-with-very-long-random-shared-key";
}

static bool hybridCloudMode() {
  return relayConfigured() && mqttBridgeConfigured();
}

static bool relayIntercomEnabled() {
  return relayConfigured() && RELAY_INTERCOM_ENABLED;
}

static bool relaySpeakerChannelEnabled() {
  return relayIntercomEnabled() && !hybridCloudMode();
}

static bool claimRelaySocket(uint32_t timeoutMs = 100) {
  if (relaySocketMutex == nullptr) {
    return true;
  }
  return xSemaphoreTakeRecursive(relaySocketMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

static void releaseRelaySocket() {
  if (relaySocketMutex != nullptr) {
    xSemaphoreGiveRecursive(relaySocketMutex);
  }
}

static bool claimMqttClient(uint32_t timeoutMs = 100) {
  if (mqttClientMutex == nullptr) {
    return true;
  }
  return xSemaphoreTakeRecursive(mqttClientMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

static void releaseMqttClient() {
  if (mqttClientMutex != nullptr) {
    xSemaphoreGiveRecursive(mqttClientMutex);
  }
}

static String relayFieldValue(const String &line, const char *key, const String &defaultValue = "") {
  String needle = String(key) + "=";
  int keyIndex = line.indexOf(needle);
  if (keyIndex < 0) {
    return defaultValue;
  }

  int valueStart = keyIndex + needle.length();
  int valueEnd = line.indexOf(' ', valueStart);
  if (valueEnd < 0) {
    valueEnd = line.length();
  }

  return line.substring(valueStart, valueEnd);
}

static bool relayFieldBool(const String &line, const char *key, bool defaultValue) {
  String value = relayFieldValue(line, key, defaultValue ? "1" : "0");
  value.toLowerCase();
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

static int relayFieldInt(const String &line, const char *key, int defaultValue, int minValue, int maxValue) {
  int value = relayFieldValue(line, key, String(defaultValue)).toInt();
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

static String relaySafeValue(const String &value) {
  String sanitized = value;
  sanitized.replace(" ", "_");
  sanitized.replace("\t", "_");
  sanitized.replace("\r", "_");
  sanitized.replace("\n", "_");
  sanitized.replace("\"", "");
  return sanitized;
}

static String relayUrlEncode(const String &value) {
  const char *hex = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); i++) {
    uint8_t c = static_cast<uint8_t>(value[i]);
    bool keep = (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~';
    if (keep) {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0f];
      encoded += hex[c & 0x0f];
    }
  }
  return encoded;
}

static String relayBuildPath(const char *channel) {
  String path = RELAY_BASE_PATH;
  if (path.isEmpty()) {
    path = "/";
  } else {
    if (!path.startsWith("/")) {
      path = "/" + path;
    }
    if (!path.endsWith("/")) {
      path += "/";
    }
  }
  path += "ws/device/";
  path += channel;
  path += "?deviceId=";
  path += relayUrlEncode(RELAY_DEVICE_ID);
  path += "&deviceToken=";
  path += relayUrlEncode(RELAY_DEVICE_TOKEN);
  return path;
}

static bool hasPsramHeap() {
  return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0;
}

static bool claimSpeaker(uint32_t timeoutMs = 200) {
  if (speakerUseMutex == nullptr) {
    return true;
  }
  return xSemaphoreTake(speakerUseMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

static void releaseSpeaker() {
  if (speakerUseMutex != nullptr) {
    xSemaphoreGive(speakerUseMutex);
  }
}

static void stopSpeakerChannelHardware() {
  if (speakerReady) {
    i2s_zero_dma_buffer(SPEAKER_I2S_PORT);
    i2s_stop(SPEAKER_I2S_PORT);
    i2s_driver_uninstall(SPEAKER_I2S_PORT);
  }
  speakerReady = false;
}

static int16_t clampToInt16(int32_t sample) {
  if (sample > INT16_MAX) {
    return INT16_MAX;
  }
  if (sample < INT16_MIN) {
    return INT16_MIN;
  }
  return static_cast<int16_t>(sample);
}

static int queryInt(WebServer &webServer, const char *name, int defaultValue, int minValue, int maxValue) {
  if (!webServer.hasArg(name)) {
    return defaultValue;
  }

  int value = webServer.arg(name).toInt();
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

static bool initMicrophone() {
  if (microphoneReady) {
    return true;
  }

  pinMode(MIC_PIN_LR, OUTPUT);
  digitalWrite(MIC_PIN_LR, LOW);

  i2s_config_t micConfig = {};
  micConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  micConfig.sample_rate = AUDIO_SAMPLE_RATE;
  micConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  micConfig.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  micConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  micConfig.intr_alloc_flags = 0;
  micConfig.dma_buf_count = 6;
  micConfig.dma_buf_len = AUDIO_FRAMES;
  micConfig.use_apll = false;
  micConfig.tx_desc_auto_clear = false;
  micConfig.fixed_mclk = 0;
  micConfig.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  micConfig.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &micConfig, 0, nullptr);
  if (err != ESP_OK) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "PDM channel failed: 0x%04x", err);
    microphoneStatus = buffer;
    Serial.println(microphoneStatus);
    return false;
  }

  i2s_pin_config_t pinConfig = {};
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
  pinConfig.bck_io_num = MIC_PIN_CLK;
  pinConfig.ws_io_num = I2S_PIN_NO_CHANGE;
  pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
  pinConfig.data_in_num = MIC_PIN_DATA;

  err = i2s_set_pin(MIC_I2S_PORT, &pinConfig);
  if (err != ESP_OK) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "PDM init failed: 0x%04x", err);
    microphoneStatus = buffer;
    Serial.println(microphoneStatus);
    i2s_driver_uninstall(MIC_I2S_PORT);
    return false;
  }

  err = i2s_set_pdm_rx_down_sample(MIC_I2S_PORT, I2S_PDM_DSR_8S);
  if (err != ESP_OK) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "PDM downsample failed: 0x%04x", err);
    microphoneStatus = buffer;
    Serial.println(microphoneStatus);
    i2s_driver_uninstall(MIC_I2S_PORT);
    return false;
  }

  microphoneReady = true;
  microphoneStatus = "ok";
  Serial.print("Microphone initialized: ");
  Serial.println(MIC_PINSET_NAME);
  return true;
}

static void stopMicrophone() {
  if (!microphoneReady) {
    return;
  }

  i2s_stop(MIC_I2S_PORT);
  i2s_driver_uninstall(MIC_I2S_PORT);
  microphoneReady = false;
  microphoneStatus = "standby";
  lastMicrophoneReadError = ESP_OK;
  lastMicrophoneBytesRead = 0;
  Serial.println("Microphone stopped");
}

static esp_err_t readMicrophoneSamples(int16_t *samples,
                                       size_t sampleCount,
                                       size_t *bytesRead,
                                       uint32_t timeoutMs) {
  *bytesRead = 0;
  if (!microphoneReady) {
    lastMicrophoneReadError = ESP_ERR_INVALID_STATE;
    lastMicrophoneBytesRead = 0;
    return lastMicrophoneReadError;
  }

  esp_err_t err = i2s_read(MIC_I2S_PORT,
                           samples,
                           sampleCount * sizeof(samples[0]),
                           bytesRead,
                           pdMS_TO_TICKS(timeoutMs));
  lastMicrophoneReadError = err;
  lastMicrophoneBytesRead = *bytesRead;
  return err;
}

static void printMicrophoneSelfTest(const char *label) {
  if (!initMicrophone()) {
    Serial.print("Microphone self-test failed to init: ");
    Serial.println(label);
    return;
  }

  int16_t samples[AUDIO_FRAMES];
  size_t bytesRead = 0;
  esp_err_t err = readMicrophoneSamples(samples, AUDIO_FRAMES, &bytesRead, 1000);
  Serial.printf("Microphone self-test (%s): err=0x%04x bytes=%u frames=%u camera=%s\n",
                label,
                static_cast<unsigned>(err),
                static_cast<unsigned>(bytesRead),
                static_cast<unsigned>(bytesRead / sizeof(samples[0])),
                cameraReady ? "ready" : "stopped");
}

static bool initSpeaker() {
  if (speakerReady) {
    return true;
  }

  pinMode(SPEAKER_PIN_PA_CTRL, OUTPUT);
  digitalWrite(SPEAKER_PIN_PA_CTRL, HIGH);
  delay(20);

  i2s_config_t speakerConfig = {};
  speakerConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  speakerConfig.sample_rate = SPEAKER_SAMPLE_RATE;
  speakerConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  speakerConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  speakerConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  speakerConfig.intr_alloc_flags = 0;
  speakerConfig.dma_buf_count = 4;
  speakerConfig.dma_buf_len = SPEAKER_FRAMES;
  speakerConfig.use_apll = false;
  speakerConfig.tx_desc_auto_clear = true;
  speakerConfig.fixed_mclk = 0;
  speakerConfig.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  speakerConfig.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

  esp_err_t err = i2s_driver_install(SPEAKER_I2S_PORT, &speakerConfig, 0, nullptr);
  if (err != ESP_OK) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "Speaker channel failed: 0x%04x", err);
    speakerStatus = buffer;
    Serial.println(speakerStatus);
    digitalWrite(SPEAKER_PIN_PA_CTRL, LOW);
    return false;
  }

  i2s_pin_config_t pinConfig = {};
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
  pinConfig.bck_io_num = SPEAKER_PIN_BCLK;
  pinConfig.ws_io_num = SPEAKER_PIN_LRCLK;
  pinConfig.data_out_num = SPEAKER_PIN_DOUT;
  pinConfig.data_in_num = I2S_PIN_NO_CHANGE;

  err = i2s_set_pin(SPEAKER_I2S_PORT, &pinConfig);
  if (err != ESP_OK) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "Speaker init failed: 0x%04x", err);
    speakerStatus = buffer;
    Serial.println(speakerStatus);
    i2s_driver_uninstall(SPEAKER_I2S_PORT);
    digitalWrite(SPEAKER_PIN_PA_CTRL, LOW);
    return false;
  }

  err = i2s_zero_dma_buffer(SPEAKER_I2S_PORT);
  if (err != ESP_OK) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "Speaker enable failed: 0x%04x", err);
    speakerStatus = buffer;
    Serial.println(speakerStatus);
    i2s_driver_uninstall(SPEAKER_I2S_PORT);
    digitalWrite(SPEAKER_PIN_PA_CTRL, LOW);
    return false;
  }

  speakerReady = true;
  speakerStatus = "ok";
  Serial.print("Speaker initialized: ");
  Serial.println(SPEAKER_PINSET_NAME);
  return true;
}

static void speakerSilence(uint16_t frames = SPEAKER_FRAMES) {
  if (!speakerReady) {
    return;
  }

  int16_t silence[SPEAKER_FRAMES * 2] = {};
  size_t framesLeft = frames;
  while (framesLeft > 0) {
    size_t framesToWrite = min(framesLeft, SPEAKER_FRAMES);
    size_t bytesWritten = 0;
    i2s_write(SPEAKER_I2S_PORT,
              silence,
              framesToWrite * 2 * sizeof(silence[0]),
              &bytesWritten,
              pdMS_TO_TICKS(200));
    framesLeft -= framesToWrite;
  }
}

static bool writeSpeakerMonoPcm(const int16_t *monoSamples, size_t frameCount, int gain = 1) {
  if (!speakerReady || monoSamples == nullptr || frameCount == 0) {
    return false;
  }

  int16_t stereoSamples[SPEAKER_FRAMES * 2];
  size_t offset = 0;
  while (offset < frameCount) {
    size_t chunkFrames = min(frameCount - offset, SPEAKER_FRAMES);
    for (size_t frame = 0; frame < chunkFrames; frame++) {
      int16_t mono = clampToInt16(static_cast<int32_t>(monoSamples[offset + frame]) * gain);
      stereoSamples[frame * 2] = mono;
      stereoSamples[frame * 2 + 1] = mono;
    }

    size_t bytesWritten = 0;
    esp_err_t err = i2s_write(SPEAKER_I2S_PORT,
                              stereoSamples,
                              chunkFrames * 2 * sizeof(stereoSamples[0]),
                              &bytesWritten,
                              pdMS_TO_TICKS(200));
    if (err != ESP_OK || bytesWritten == 0) {
      return false;
    }
    offset += chunkFrames;
  }

  return true;
}

static void writeWavHeader(WiFiClient &client, uint32_t dataSize = 0x7ffff000UL) {
  constexpr uint16_t channels = 1;
  constexpr uint16_t bitsPerSample = 16;
  constexpr uint32_t byteRate = AUDIO_SAMPLE_RATE * channels * bitsPerSample / 8;
  constexpr uint16_t blockAlign = channels * bitsPerSample / 8;
  uint32_t riffSize = dataSize + 36;

  uint8_t header[44] = {
      'R', 'I', 'F', 'F',
      static_cast<uint8_t>(riffSize),
      static_cast<uint8_t>(riffSize >> 8),
      static_cast<uint8_t>(riffSize >> 16),
      static_cast<uint8_t>(riffSize >> 24),
      'W', 'A', 'V', 'E',
      'f', 'm', 't', ' ',
      16, 0, 0, 0,
      1, 0,
      static_cast<uint8_t>(channels),
      static_cast<uint8_t>(channels >> 8),
      static_cast<uint8_t>(AUDIO_SAMPLE_RATE),
      static_cast<uint8_t>(AUDIO_SAMPLE_RATE >> 8),
      static_cast<uint8_t>(AUDIO_SAMPLE_RATE >> 16),
      static_cast<uint8_t>(AUDIO_SAMPLE_RATE >> 24),
      static_cast<uint8_t>(byteRate),
      static_cast<uint8_t>(byteRate >> 8),
      static_cast<uint8_t>(byteRate >> 16),
      static_cast<uint8_t>(byteRate >> 24),
      static_cast<uint8_t>(blockAlign),
      static_cast<uint8_t>(blockAlign >> 8),
      static_cast<uint8_t>(bitsPerSample),
      static_cast<uint8_t>(bitsPerSample >> 8),
      'd', 'a', 't', 'a',
      static_cast<uint8_t>(dataSize),
      static_cast<uint8_t>(dataSize >> 8),
      static_cast<uint8_t>(dataSize >> 16),
      static_cast<uint8_t>(dataSize >> 24),
  };

  client.write(header, sizeof(header));
}

static void sendAudioNotFound(WiFiClient &client) {
  client.println("HTTP/1.1 404 Not Found");
  client.println("Content-Type: text/plain; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.println("Not found");
}

struct AudioHttpRequest {
  String path;
  String websocketKey;
  bool websocketUpgrade = false;
};

static AudioHttpRequest readAudioRequest(WiFiClient &client) {
  AudioHttpRequest request;
  client.setTimeout(1000);
  String requestLine = client.readStringUntil('\n');
  requestLine.trim();

  int firstSpace = requestLine.indexOf(' ');
  int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
  if (!requestLine.startsWith("GET ") || firstSpace < 0 || secondSpace < 0) {
    return request;
  }

  request.path = requestLine.substring(firstSpace + 1, secondSpace);

  while (client.connected()) {
    String headerLine = client.readStringUntil('\n');
    if (headerLine == "\r" || headerLine.length() == 0) {
      break;
    }

    headerLine.trim();
    int colonIndex = headerLine.indexOf(':');
    if (colonIndex < 0) {
      continue;
    }

    String name = headerLine.substring(0, colonIndex);
    String value = headerLine.substring(colonIndex + 1);
    name.toLowerCase();
    value.trim();

    if (name == "sec-websocket-key") {
      request.websocketKey = value;
    } else if (name == "upgrade" && value.equalsIgnoreCase("websocket")) {
      request.websocketUpgrade = true;
    }
  }

  return request;
}

static String webSocketAcceptKey(const String &clientKey) {
  static constexpr char WEBSOCKET_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  String combined = clientKey + WEBSOCKET_GUID;
  unsigned char sha1Hash[20] = {};
  unsigned char encoded[32] = {};
  size_t encodedLength = 0;

  mbedtls_sha1(reinterpret_cast<const unsigned char *>(combined.c_str()), combined.length(), sha1Hash);
  if (mbedtls_base64_encode(encoded, sizeof(encoded) - 1, &encodedLength, sha1Hash, sizeof(sha1Hash)) != 0) {
    return "";
  }

  encoded[encodedLength] = '\0';
  return String(reinterpret_cast<char *>(encoded));
}

static bool sendWebSocketBinary(WiFiClient &client, const uint8_t *payload, size_t length) {
  if (!client.connected()) {
    return false;
  }

  uint8_t header[4] = {};
  size_t headerLength = 0;
  header[0] = 0x82;
  if (length <= 125) {
    header[1] = static_cast<uint8_t>(length);
    headerLength = 2;
  } else if (length <= 0xffff) {
    header[1] = 126;
    header[2] = static_cast<uint8_t>(length >> 8);
    header[3] = static_cast<uint8_t>(length);
    headerLength = 4;
  } else {
    return false;
  }

  if (client.write(header, headerLength) != headerLength) {
    return false;
  }
  return client.write(payload, length) == length;
}

static bool sendWebSocketControl(WiFiClient &client, uint8_t opcode, const uint8_t *payload = nullptr, size_t length = 0) {
  if (!client.connected() || length > 125) {
    return false;
  }

  uint8_t header[2] = {
      static_cast<uint8_t>(0x80 | (opcode & 0x0f)),
      static_cast<uint8_t>(length),
  };
  if (client.write(header, sizeof(header)) != sizeof(header)) {
    return false;
  }
  if (length == 0) {
    return true;
  }
  return client.write(payload, length) == length;
}

static bool discardClientBytes(WiFiClient &client, size_t length) {
  uint8_t scratch[64];
  size_t remaining = length;
  while (remaining > 0) {
    size_t chunk = min(remaining, sizeof(scratch));
    if (client.readBytes(reinterpret_cast<char *>(scratch), chunk) != chunk) {
      return false;
    }
    remaining -= chunk;
  }
  return true;
}

static bool readWebSocketFrame(WiFiClient &client,
                               uint8_t &opcode,
                               uint8_t *payload,
                               size_t capacity,
                               size_t &payloadLength) {
  payloadLength = 0;

  uint8_t header[2] = {};
  if (client.readBytes(reinterpret_cast<char *>(header), sizeof(header)) != sizeof(header)) {
    return false;
  }

  opcode = header[0] & 0x0f;
  bool masked = (header[1] & 0x80) != 0;
  uint64_t length = header[1] & 0x7f;
  if (length == 126) {
    uint8_t extended[2] = {};
    if (client.readBytes(reinterpret_cast<char *>(extended), sizeof(extended)) != sizeof(extended)) {
      return false;
    }
    length = (static_cast<uint16_t>(extended[0]) << 8) | extended[1];
  } else if (length == 127) {
    uint8_t extended[8] = {};
    if (client.readBytes(reinterpret_cast<char *>(extended), sizeof(extended)) != sizeof(extended)) {
      return false;
    }
    length = 0;
    for (size_t i = 0; i < sizeof(extended); i++) {
      length = (length << 8) | extended[i];
    }
  }

  uint8_t mask[4] = {};
  if (masked && client.readBytes(reinterpret_cast<char *>(mask), sizeof(mask)) != sizeof(mask)) {
    return false;
  }

  if (length > capacity) {
    return discardClientBytes(client, static_cast<size_t>(length));
  }

  payloadLength = static_cast<size_t>(length);
  if (payloadLength > 0 &&
      client.readBytes(reinterpret_cast<char *>(payload), payloadLength) != payloadLength) {
    return false;
  }

  if (masked) {
    for (size_t i = 0; i < payloadLength; i++) {
      payload[i] ^= mask[i & 0x03];
    }
  }

  return true;
}

static int audioQueryInt(const String &path, const char *name, int defaultValue, int minValue, int maxValue) {
  String key = String(name) + "=";
  int keyIndex = path.indexOf(key);
  if (keyIndex < 0) {
    return defaultValue;
  }

  int valueStart = keyIndex + key.length();
  int valueEnd = path.indexOf('&', valueStart);
  if (valueEnd < 0) {
    valueEnd = path.length();
  }

  int value = path.substring(valueStart, valueEnd).toInt();
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

static bool audioUseRightChannel(const String &path) {
  return path.indexOf("ch=right") >= 0 || path.indexOf("channel=right") >= 0;
}

static size_t convertMicFramesToPcm(const int16_t *rawSamples,
                                    size_t frameCount,
                                    bool useRightChannel,
                                    int sampleShift,
                                    int sampleGain,
                                    int32_t &dcEstimate,
                                    int16_t *pcmSamples) {
  digitalWrite(MIC_PIN_LR, useRightChannel ? HIGH : LOW);

  for (size_t frame = 0; frame < frameCount; frame++) {
    int32_t selected = rawSamples[frame];
    dcEstimate += (selected - dcEstimate) >> 8;
    int32_t filtered = selected - dcEstimate;
    int32_t scaled = (filtered >> sampleShift) * sampleGain;
    pcmSamples[frame] = clampToInt16(scaled);
  }

  return frameCount;
}

static uint32_t abs32u(int32_t value) {
  if (value == INT32_MIN) {
    return static_cast<uint32_t>(INT32_MAX);
  }
  return static_cast<uint32_t>(value < 0 ? -value : value);
}

static void serveAudioDebug(WiFiClient &client) {
  if (!microphoneReady && !initMicrophone()) {
    client.println("HTTP/1.1 503 Service Unavailable");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println(microphoneStatus);
    client.stop();
    return;
  }

  int16_t rawSamples[AUDIO_FRAMES];
  int32_t minSample = INT32_MAX;
  int32_t maxSample = INT32_MIN;
  uint64_t sumAbs = 0;
  size_t frameCountTotal = 0;
  uint16_t readCalls = 0;
  uint16_t okReads = 0;
  uint16_t zeroReads = 0;
  esp_err_t lastErr = ESP_OK;
  size_t lastBytes = 0;

  for (uint8_t block = 0; block < 8; block++) {
    size_t bytesRead = 0;
    readCalls++;
    esp_err_t err = readMicrophoneSamples(rawSamples,
                                          AUDIO_FRAMES,
                                          &bytesRead,
                                          1000);
    lastErr = err;
    lastBytes = bytesRead;
    if (err != ESP_OK || bytesRead == 0) {
      if (bytesRead == 0) {
        zeroReads++;
      }
      continue;
    }
    okReads++;

    size_t frameCount = bytesRead / sizeof(rawSamples[0]);
    if (frameCount > AUDIO_FRAMES) {
      frameCount = AUDIO_FRAMES;
    }

    for (size_t frame = 0; frame < frameCount; frame++) {
      int32_t sample = rawSamples[frame];
      minSample = min(minSample, sample);
      maxSample = max(maxSample, sample);
      sumAbs += abs32u(sample);
    }

    frameCountTotal += frameCount;
  }

  String text;
  text.reserve(512);
  text += "Microphone: " + microphoneStatus + "\n";
  text += "Frames: " + String(frameCountTotal) + "\n";
  text += "Read calls/ok/zero: " + String(readCalls) + " / " + String(okReads) + " / " + String(zeroReads) + "\n";
  text += "Last err: 0x" + String(static_cast<uint32_t>(lastErr), HEX) + "\n";
  text += "Last bytes: " + String(lastBytes) + "\n";
  text += "Camera ready during debug: " + String(cameraReady ? "true" : "false") + "\n";
  text += "Format: PDM mic -> 16-bit PCM, 16 kHz\n";
  if (frameCountTotal > 0) {
    text += "Sample min/max/avg_abs: " + String(minSample) + " / " + String(maxSample) + " / " + String(static_cast<uint32_t>(sumAbs / frameCountTotal)) + "\n";
  }
  text += "Try: /audio.wav?ch=left&shift=0&gain=4\n";
  text += "Try: /audio.wav?ch=right&shift=0&gain=4\n";
  text += "Low latency: ws://<ip>:81/audio.ws?ch=left&shift=0&gain=4\n";
  text += "Volume: increase gain to 6..12, or decrease it if distorted\n";

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/plain; charset=utf-8");
  client.println("Cache-Control: no-store");
  client.println("Connection: close");
  client.println();
  client.print(text);
  client.stop();
}

static void serveAudioSample(WiFiClient &client, const String &path) {
  if (!microphoneReady && !initMicrophone()) {
    client.println("HTTP/1.1 503 Service Unavailable");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println(microphoneStatus);
    client.stop();
    return;
  }

  bool useRightChannel = audioUseRightChannel(path);
  int sampleShift = audioQueryInt(path, "shift", AUDIO_SAMPLE_SHIFT, 0, 8);
  int sampleGain = audioQueryInt(path, "gain", AUDIO_SAMPLE_GAIN, 1, 16);
  uint32_t durationMs = static_cast<uint32_t>(audioQueryInt(path, "ms", 3000, 500, 8000));
  uint32_t samplesTotal = (AUDIO_SAMPLE_RATE * durationMs) / 1000;
  uint32_t dataSize = samplesTotal * sizeof(int16_t);

  Serial.printf("HTTP audio sample channel=%s shift=%d gain=%d ms=%lu\n",
                useRightChannel ? "right" : "left",
                sampleShift,
                sampleGain,
                static_cast<unsigned long>(durationMs));

  client.setNoDelay(true);
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: audio/wav");
  client.println("Cache-Control: no-store");
  client.printf("Content-Length: %lu\r\n", static_cast<unsigned long>(sizeof(uint8_t) * 44 + dataSize));
  client.println("Connection: close");
  client.println();
  writeWavHeader(client, dataSize);

  int16_t rawSamples[AUDIO_FRAMES];
  int16_t pcmSamples[AUDIO_FRAMES];
  int32_t dcEstimate = 0;
  uint32_t samplesWritten = 0;

  while (client.connected() && samplesWritten < samplesTotal) {
    size_t bytesRead = 0;
    esp_err_t err = readMicrophoneSamples(rawSamples,
                                          AUDIO_FRAMES,
                                          &bytesRead,
                                          1000);
    if (err != ESP_OK || bytesRead == 0) {
      delay(2);
      continue;
    }

    size_t frameCount = bytesRead / sizeof(rawSamples[0]);
    if (frameCount > AUDIO_FRAMES) {
      frameCount = AUDIO_FRAMES;
    }
    if (samplesWritten + frameCount > samplesTotal) {
      frameCount = samplesTotal - samplesWritten;
    }

    convertMicFramesToPcm(rawSamples, frameCount, useRightChannel, sampleShift, sampleGain, dcEstimate, pcmSamples);
    size_t bytesToWrite = frameCount * sizeof(pcmSamples[0]);
    if (bytesToWrite > 0 && client.write(reinterpret_cast<uint8_t *>(pcmSamples), bytesToWrite) == 0) {
      break;
    }

    samplesWritten += frameCount;
    yield();
  }

  client.stop();
}

static void serveAudioWebSocket(WiFiClient &client, const AudioHttpRequest &request) {
  if (!request.websocketUpgrade || request.websocketKey.length() == 0) {
    client.println("HTTP/1.1 400 Bad Request");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println("Expected WebSocket upgrade");
    client.stop();
    return;
  }

  if (!microphoneReady && !initMicrophone()) {
    client.println("HTTP/1.1 503 Service Unavailable");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println(microphoneStatus);
    client.stop();
    return;
  }

  bool useRightChannel = audioUseRightChannel(request.path);
  int sampleShift = audioQueryInt(request.path, "shift", AUDIO_SAMPLE_SHIFT, 0, 8);
  int sampleGain = audioQueryInt(request.path, "gain", AUDIO_SAMPLE_GAIN, 1, 16);
  String acceptKey = webSocketAcceptKey(request.websocketKey);
  if (acceptKey.length() == 0) {
    client.println("HTTP/1.1 500 Internal Server Error");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println("WebSocket accept failed");
    client.stop();
    return;
  }

  Serial.printf("HTTP audio /audio.ws channel=%s shift=%d gain=%d\n",
                useRightChannel ? "right" : "left",
                sampleShift,
                sampleGain);

  client.setNoDelay(true);
  client.println("HTTP/1.1 101 Switching Protocols");
  client.println("Upgrade: websocket");
  client.println("Connection: Upgrade");
  client.print("Sec-WebSocket-Accept: ");
  client.println(acceptKey);
  client.println("Cache-Control: no-store");
  client.println();

  int16_t rawSamples[AUDIO_FRAMES];
  int16_t pcmSamples[AUDIO_FRAMES];
  int32_t dcEstimate = 0;

  while (client.connected()) {
    int websocketOpcode = client.available() ? (client.peek() & 0x0f) : -1;
    while (client.available()) {
      client.read();
    }
    if (websocketOpcode == 0x08) {
      break;
    }

    size_t bytesRead = 0;
    esp_err_t err = readMicrophoneSamples(rawSamples,
                                          AUDIO_FRAMES,
                                          &bytesRead,
                                          200);
    if (err != ESP_OK || bytesRead == 0) {
      delay(1);
      continue;
    }

    size_t frameCount = bytesRead / sizeof(rawSamples[0]);
    if (frameCount > AUDIO_FRAMES) {
      frameCount = AUDIO_FRAMES;
    }

    convertMicFramesToPcm(rawSamples, frameCount, useRightChannel, sampleShift, sampleGain, dcEstimate, pcmSamples);
    size_t bytesToWrite = frameCount * sizeof(pcmSamples[0]);
    if (bytesToWrite > 0 && !sendWebSocketBinary(client, reinterpret_cast<uint8_t *>(pcmSamples), bytesToWrite)) {
      break;
    }

    yield();
  }

  client.stop();
  Serial.println("Audio WebSocket disconnected");
}

static void serveSpeakerWebSocket(WiFiClient &client, const AudioHttpRequest &request) {
  if (!request.websocketUpgrade || request.websocketKey.length() == 0) {
    client.println("HTTP/1.1 400 Bad Request");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println("Expected WebSocket upgrade");
    client.stop();
    return;
  }

  if (!claimSpeaker(100)) {
    client.println("HTTP/1.1 409 Conflict");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println("Speaker busy");
    client.stop();
    return;
  }

  if (!initSpeaker()) {
    releaseSpeaker();
    client.println("HTTP/1.1 503 Service Unavailable");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println(speakerStatus);
    client.stop();
    return;
  }

  int speakerGain = audioQueryInt(request.path, "gain", 2, 1, 8);
  String acceptKey = webSocketAcceptKey(request.websocketKey);
  if (acceptKey.length() == 0) {
    releaseSpeaker();
    client.println("HTTP/1.1 500 Internal Server Error");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println("WebSocket accept failed");
    client.stop();
    return;
  }

  Serial.printf("HTTP speaker /speaker.ws gain=%d\n", speakerGain);
  String previousSpeakerStatus = speakerStatus;
  speakerStatus = "browser talkback";

  client.setNoDelay(true);
  client.println("HTTP/1.1 101 Switching Protocols");
  client.println("Upgrade: websocket");
  client.println("Connection: Upgrade");
  client.print("Sec-WebSocket-Accept: ");
  client.println(acceptKey);
  client.println("Cache-Control: no-store");
  client.println();

  client.setTimeout(1000);
  int16_t monoSamples[SPEAKER_FRAMES * 4];
  while (client.connected()) {
    uint8_t opcode = 0;
    size_t payloadLength = 0;
    if (!readWebSocketFrame(client,
                            opcode,
                            reinterpret_cast<uint8_t *>(monoSamples),
                            sizeof(monoSamples),
                            payloadLength)) {
      break;
    }

    if (opcode == 0x08) {
      sendWebSocketControl(client, 0x08);
      break;
    }
    if (opcode == 0x09) {
      sendWebSocketControl(client, 0x0a, reinterpret_cast<uint8_t *>(monoSamples), payloadLength);
      continue;
    }
    if (opcode != 0x02 || payloadLength == 0 || (payloadLength % sizeof(int16_t)) != 0) {
      continue;
    }

    size_t frameCount = payloadLength / sizeof(int16_t);
    if (!writeSpeakerMonoPcm(monoSamples, frameCount, speakerGain)) {
      break;
    }
    yield();
  }

  speakerSilence(256);
  speakerStatus = previousSpeakerStatus;
  releaseSpeaker();
  client.stop();
  Serial.println("Speaker WebSocket disconnected");
}

static void serveAudioStream(WiFiClient &client) {
  AudioHttpRequest request = readAudioRequest(client);
  String path = request.path;
  if (path.startsWith("/audio/debug")) {
    serveAudioDebug(client);
    return;
  }

  if (path.startsWith("/audio.ws")) {
    serveAudioWebSocket(client, request);
    return;
  }

  if (path.startsWith("/speaker.ws")) {
    serveSpeakerWebSocket(client, request);
    return;
  }

  if (path.startsWith("/audio/sample.wav")) {
    serveAudioSample(client, path);
    return;
  }

  if (!path.startsWith("/audio.wav")) {
    sendAudioNotFound(client);
    client.stop();
    return;
  }

  if (!microphoneReady && !initMicrophone()) {
    client.println("HTTP/1.1 503 Service Unavailable");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println(microphoneStatus);
    client.stop();
    return;
  }

  bool useRightChannel = audioUseRightChannel(path);
  int sampleShift = audioQueryInt(path, "shift", AUDIO_SAMPLE_SHIFT, 0, 8);
  int sampleGain = audioQueryInt(path, "gain", AUDIO_SAMPLE_GAIN, 1, 16);

  Serial.printf("HTTP audio /audio.wav channel=%s shift=%d gain=%d\n",
                useRightChannel ? "right" : "left",
                sampleShift,
                sampleGain);
  client.setNoDelay(true);
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: audio/wav");
  client.println("Cache-Control: no-store");
  client.println("Accept-Ranges: none");
  client.println("Connection: close");
  client.println();
  writeWavHeader(client);

  int16_t rawSamples[AUDIO_FRAMES];
  int16_t pcmSamples[AUDIO_FRAMES];
  int32_t dcEstimate = 0;

  while (client.connected()) {
    size_t bytesRead = 0;
    esp_err_t err = readMicrophoneSamples(rawSamples,
                                          AUDIO_FRAMES,
                                          &bytesRead,
                                          1000);
    if (err != ESP_OK || bytesRead == 0) {
      delay(2);
      continue;
    }

    size_t frameCount = bytesRead / sizeof(rawSamples[0]);
    if (frameCount > AUDIO_FRAMES) {
      frameCount = AUDIO_FRAMES;
    }

    convertMicFramesToPcm(rawSamples, frameCount, useRightChannel, sampleShift, sampleGain, dcEstimate, pcmSamples);

    size_t bytesToWrite = frameCount * sizeof(pcmSamples[0]);
    if (bytesToWrite > 0 && client.write(reinterpret_cast<uint8_t *>(pcmSamples), bytesToWrite) == 0) {
      break;
    }

    yield();
  }

  client.stop();
  Serial.println("Audio client disconnected");
}

static void audioServerTask(void *parameter) {
  (void)parameter;

  for (;;) {
    WiFiClient client = audioServer.accept();
    if (client) {
      serveAudioStream(client);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void speakerInputServerTask(void *parameter) {
  (void)parameter;

  for (;;) {
    WiFiClient client = speakerInputServer.accept();
    if (client) {
      serveAudioStream(client);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static bool requireAuth() {
  if (server.authenticate(HTTP_AUTH_USER, HTTP_AUTH_PASSWORD)) {
    return true;
  }

  server.requestAuthentication(BASIC_AUTH, "ESP32-S3-CAM-DEN", "Authentication required");
  return false;
}

static camera_config_t makeCameraConfig() {
  camera_config_t config = {};

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAMERA_PIN_Y2;
  config.pin_d1 = CAMERA_PIN_Y3;
  config.pin_d2 = CAMERA_PIN_Y4;
  config.pin_d3 = CAMERA_PIN_Y5;
  config.pin_d4 = CAMERA_PIN_Y6;
  config.pin_d5 = CAMERA_PIN_Y7;
  config.pin_d6 = CAMERA_PIN_Y8;
  config.pin_d7 = CAMERA_PIN_Y9;
  config.pin_xclk = CAMERA_PIN_XCLK;
  config.pin_pclk = CAMERA_PIN_PCLK;
  config.pin_vsync = CAMERA_PIN_VSYNC;
  config.pin_href = CAMERA_PIN_HREF;
  config.pin_sccb_sda = CAMERA_PIN_SIOD;
  config.pin_sccb_scl = CAMERA_PIN_SIOC;
  config.pin_pwdn = CAMERA_PIN_PWDN;
  config.pin_reset = CAMERA_PIN_RESET;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = CAMERA_FRAME_SIZE;
  config.jpeg_quality = CAMERA_JPEG_QUALITY;
  config.fb_count = CAMERA_FB_COUNT;
  config.fb_location = hasPsramHeap() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  return config;
}

static void applyCameraSensorProfile(framesize_t frameSize, int jpegQuality, bool fullTuning) {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    return;
  }

  if (fullTuning || activeCameraFrameSize != frameSize) {
    sensor->set_framesize(sensor, frameSize);
    activeCameraFrameSize = frameSize;
  }
  if (fullTuning || activeCameraJpegQuality != jpegQuality) {
    sensor->set_quality(sensor, jpegQuality);
    activeCameraJpegQuality = jpegQuality;
  }
  if (fullTuning) {
    sensor->set_brightness(sensor, 0);
    sensor->set_contrast(sensor, 1);
    sensor->set_saturation(sensor, 1);
    sensor->set_gainceiling(sensor, GAINCEILING_8X);
    sensor->set_vflip(sensor, 0);
    sensor->set_hmirror(sensor, 0);
  }
}

static bool initCamera() {
  if (cameraReady) {
    lastCameraUseMs = millis();
    return true;
  }

  Serial.println("Camera starting");
  Serial.printf("Heap before camera: internal=%u largest_internal=%u psram=%u largest_psram=%u\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  camera_config_t config = makeCameraConfig();
  bool microphoneWasReady = microphoneReady;
  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK && microphoneWasReady) {
    Serial.printf("Camera init with microphone active failed: 0x%04x. Retrying and restoring microphone afterwards.\n",
                  static_cast<unsigned>(err));
    esp_camera_deinit();
    stopMicrophone();
    err = esp_camera_init(&config);
    if (err == ESP_OK) {
      if (!initMicrophone()) {
        Serial.println("Microphone re-init after camera start failed");
      }
    } else {
      initMicrophone();
    }
  }

  if (err != ESP_OK) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "Camera init failed: 0x%04x", err);
    cameraStatus = buffer;
    cameraReady = false;
    Serial.println(cameraStatus);
    esp_camera_deinit();
    return false;
  }

  applyCameraSensorProfile(CAMERA_FRAME_SIZE, CAMERA_JPEG_QUALITY, true);

  cameraReady = true;
  cameraStatus = "ok";
  lastCameraUseMs = millis();
  Serial.println("Camera initialized");
  return true;
}

static void stopCamera() {
  if (!cameraReady) {
    return;
  }

  if (KEEP_CAMERA_INITIALIZED) {
    lastCameraUseMs = millis();
    cameraStatus = "ok";
    Serial.println("Camera kept initialized");
    return;
  }

  esp_camera_deinit();
  cameraReady = false;
  lastCameraUseMs = 0;
  cameraStatus = "standby";
  Serial.println("Camera stopped");
}

static String currentStaIp() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "not connected";
}

static String currentApIp() {
  wifi_mode_t mode = WiFi.getMode();
  bool apEnabled = mode == WIFI_AP || mode == WIFI_AP_STA;
  return apEnabled ? WiFi.softAPIP().toString() : "disabled";
}

static String htmlPage() {
  String html;
  html.reserve(11200);

  html += F("<!doctype html><html lang='uk'><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>ESP32-S3-CAM-DEN</title>");
  html += F("<style>");
  html += F(":root{color-scheme:dark}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,sans-serif;background:#0d1117;color:#e6edf3}");
  html += F("main{max-width:980px;margin:0 auto;padding:22px}.top{display:flex;align-items:center;justify-content:space-between;gap:14px;flex-wrap:wrap}");
  html += F("h1{font-size:28px;margin:0}.muted{color:#8b949e}.pill{border:1px solid #30363d;background:#161b22;border-radius:999px;padding:8px 12px}");
  html += F(".actions{display:flex;gap:10px;flex-wrap:wrap;margin:18px 0}button,a.btn{border:0;border-radius:8px;background:#2f81f7;color:#fff;padding:10px 14px;font-weight:700;text-decoration:none;cursor:pointer}");
  html += F("button.secondary,a.secondary{background:#30363d}.viewer{background:#010409;border:1px solid #30363d;border-radius:12px;min-height:300px;display:grid;place-items:center;overflow:hidden}");
  html += F("img{max-width:100%;height:auto;display:block}audio{width:100%;margin-top:8px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px;margin-top:16px}");
  html += F(".box{border:1px solid #30363d;border-radius:10px;background:#161b22;padding:12px}code{color:#79c0ff;word-break:break-all}");
  html += F("</style></head><body><main>");
  html += F("<div class='top'><div><h1>ESP32-S3-CAM-DEN</h1><div class='muted'>OV2640 snapshot С‚Р° MJPEG stream</div></div>");
  html += F("<div class='pill'>РљР°РјРµСЂР°: <code id='state'>");
  html += cameraStatus;
  html += F("</code></div></div>");
  html += F("<div class='actions'>");
  html += F("<button onclick='snapshot()'>Snapshot</button>");
  html += F("<button class='secondary' onclick='startLive()'>Start live A/V</button>");
  html += F("<button class='secondary' onclick='stopView()'>Stop</button>");
  html += F("<a class='btn secondary' href='/capture' target='_blank'>Open JPEG</a>");
  html += F("<a class='btn secondary' href='/health' target='_blank'>Health</a>");
  html += F("</div>");
  html += F("<div class='viewer'><img id='view' alt='РќР°С‚РёСЃРЅС–С‚СЊ Snapshot Р°Р±Рѕ Start stream'></div>");
  html += F("<div class='grid'>");
  html += F("<div class='box'>РњС–РєСЂРѕС„РѕРЅ<br><code>");
  html += microphoneStatus;
  html += F("</code><audio id='audio' controls preload='none' playsinline></audio>");
  html += F("<div class='actions'><button class='secondary' onclick=\"chooseLiveAudio('left',4)\">Live left</button>");
  html += F("<button class='secondary' onclick=\"chooseLiveAudio('right',4)\">Live right</button>");
  html += F("<button class='secondary' onclick=\"chooseLiveAudio('left',8)\">Louder live</button>");
  html += F("<button class='secondary' onclick='stopWsAudio()'>Stop audio</button>");
  html += F("<button class='secondary' onclick=\"setAudio('left',4)\">WAV left</button>");
  html += F("<button class='secondary' onclick=\"setAudio('right',4)\">WAV right</button></div>");
  html += F("<div class='muted'>Desktop uses low-latency WebSocket audio. Phones fall back to the browser audio player after you tap a Live button.</div></div>");
  html += F("<div class='box'>Speaker / mic tests<br><code>");
  html += speakerStatus;
  html += F("</code><div class='actions'>");
  html += F("<button class='secondary' onclick='speakerTone()'>Speaker tone</button>");
  html += F("<button class='secondary' onclick='startTalkback(2)'>Browser mic -> speaker</button>");
  html += F("<button class='secondary' onclick='startTalkback(4)'>Louder talkback</button>");
  html += F("<button class='secondary' onclick='stopTalkback()'>Stop browser speaker</button>");
  html += F("<button class='secondary' onclick=\"micSpeaker('left')\">Micв†’speaker left</button>");
  html += F("<button class='secondary' onclick=\"micSpeaker('right')\">Micв†’speaker right</button>");
  html += F("<button class='secondary' onclick=\"sampleAudio('left')\">Download mic WAV</button>");
  html += F("<button class='secondary' onclick='audioDebug()'>Mic debug</button>");
  html += F("</div><div class='muted'>Browser talkback uses your phone/PC microphone and sends it to the board speaker. Modern phone/Chrome browsers require HTTPS or localhost for microphone capture.</div></div>");
  html += F("<div class='box'>Router IP<br><code>");
  html += currentStaIp();
  html += F("</code></div><div class='box'>Direct AP IP<br><code>");
  html += currentApIp();
  html += F("</code></div><div class='box'>Cloud relay<br><code>");
  html += relayStatus;
  html += F("</code>");
  if (relayConfigured()) {
    html += F("<div class='muted'>");
    html += RELAY_HOST;
    html += F("</div>");
  }
  html += F("</div><div class='box'>MQTT bridge<br><code>");
  html += mqttStatus;
  html += F("</code>");
  if (mqttBridgeConfigured()) {
    html += F("<div class='muted'>");
    html += MQTT_BROKER_HOST;
    html += F("</div>");
  }
  html += F("</div><div class='box'>PSRAM<br><code>");
  html += hasPsramHeap() ? "available" : "not available, using DRAM";
  html += F("</code></div><div class='box'>Pins<br><code>");
  html += CAMERA_PINSET_NAME;
  html += F("</code></div><div class='box'>Frame<br><code>");
  html += F("QVGA JPEG, 1 fb");
  html += F("</code></div></div>");
  html += F("<script>");
  html += F("const img=document.getElementById('view');");
  html += F("const aud=document.getElementById('audio');");
  html += F("let wsAudio=null,audioCtx=null,nextAudioTime=0,pendingAudioStart=null,liveAudioChannel='left',liveAudioGain=4,talkbackWs=null,talkbackStream=null,talkbackSource=null,talkbackProcessor=null,talkbackSink=null,wsAudioFrames=0,wsAudioFallbackTimer=null;");
  html += F("function isMobileBrowser(){return /Android|iPhone|iPad|iPod|Mobile/i.test(navigator.userAgent||'')}");
  html += F("function audioUrl(ch,gain){return 'http://'+location.hostname+':");
  html += AUDIO_PORT;
  html += F("/audio.wav?ch='+ch+'&shift=0&gain='+gain+'&cache='+Date.now()}");
  html += F("function audioSampleUrl(ch){return 'http://'+location.hostname+':");
  html += AUDIO_PORT;
  html += F("/audio/sample.wav?ch='+ch+'&shift=0&gain=4&ms=3000&cache='+Date.now()}");
  html += F("function speakerWsUrl(gain){return 'ws://'+location.hostname+':");
  html += SPEAKER_INPUT_PORT;
  html += F("/speaker.ws?gain='+gain+'&cache='+Date.now()}");
  html += F("function audioDebug(){window.open('http://'+location.hostname+':");
  html += AUDIO_PORT;
  html += F("/audio/debug','_blank')}");
  html += F("function ensureAudioContext(){const AC=window.AudioContext||window.webkitAudioContext;if(!AC){alert('WebAudio not supported');return null}if(!audioCtx){try{audioCtx=new AC({sampleRate:16000})}catch(e){audioCtx=new AC()}}return audioCtx}");
  html += F("function requestBrowserMicrophone(){const legacy=navigator.getUserMedia||navigator.webkitGetUserMedia||navigator.mozGetUserMedia;if(navigator.mediaDevices&&navigator.mediaDevices.getUserMedia){return navigator.mediaDevices.getUserMedia({audio:{channelCount:1,echoCancellation:false,noiseSuppression:false,autoGainControl:false}})}if(legacy){return new Promise((resolve,reject)=>legacy.call(navigator,{audio:{channelCount:1,echoCancellation:false,noiseSuppression:false,autoGainControl:false}},resolve,reject))}if(!window.isSecureContext){throw new Error('secure-context-required')}throw new Error('unsupported')}");
  html += F("function downsampleToInt16(input,inputRate,targetRate){if(!input||input.length===0)return new Int16Array(0);if(targetRate===inputRate){const pcm=new Int16Array(input.length);for(let i=0;i<input.length;i++){const s=Math.max(-1,Math.min(1,input[i]));pcm[i]=s<0?s*32768:s*32767}return pcm}const ratio=inputRate/targetRate;const outLength=Math.max(1,Math.floor(input.length/ratio));const pcm=new Int16Array(outLength);let offset=0;for(let i=0;i<outLength;i++){const next=Math.min(input.length,Math.floor((i+1)*ratio));let sum=0,count=0;for(let j=offset;j<next;j++){sum+=input[j];count++}const sample=count?sum/count:input[Math.min(offset,input.length-1)];const s=Math.max(-1,Math.min(1,sample));pcm[i]=s<0?s*32768:s*32767;offset=Math.max(next,offset+1)}return pcm}");
  html += F("function clearWsAudioFallback(){if(wsAudioFallbackTimer){clearTimeout(wsAudioFallbackTimer);wsAudioFallbackTimer=null}}");
  html += F("function stopWsAudio(){pendingAudioStart=null;clearWsAudioFallback();if(wsAudio){const ws=wsAudio;wsAudio=null;ws.onclose=null;try{ws.close()}catch(e){}}}");
  html += F("async function startWsAudio(ch,gain,allowFallback){liveAudioChannel=ch;liveAudioGain=gain;stopWsAudio();aud.pause();const ctx=ensureAudioContext();if(!ctx)return;try{await ctx.resume()}catch(e){}if(ctx.state!=='running'){pendingAudioStart={ch,gain};return}pendingAudioStart=null;nextAudioTime=ctx.currentTime+0.04;wsAudioFrames=0;wsAudio=new WebSocket('ws://'+location.hostname+':");
  html += AUDIO_PORT;
  html += F("/audio.ws?ch='+ch+'&shift=0&gain='+gain+'&cache='+Date.now());wsAudio.binaryType='arraybuffer';wsAudioFallbackTimer=setTimeout(()=>{if(wsAudio&&wsAudioFrames===0&&allowFallback){stopWsAudio();setAudio(ch,gain)}},1500);wsAudio.onmessage=e=>{wsAudioFrames++;if(wsAudioFrames===1)clearWsAudioFallback();const view=new DataView(e.data);const count=view.byteLength/2;const buf=ctx.createBuffer(1,count,16000);const out=buf.getChannelData(0);for(let i=0;i<count;i++){out[i]=Math.max(-1,Math.min(1,view.getInt16(i*2,true)/32768))}const src=ctx.createBufferSource();src.buffer=buf;src.connect(ctx.destination);const now=ctx.currentTime;if(nextAudioTime<now+0.02)nextAudioTime=now+0.02;src.start(nextAudioTime);nextAudioTime+=count/16000;if(nextAudioTime>now+0.25)nextAudioTime=now+0.08};wsAudio.onclose=()=>{const shouldFallback=allowFallback&&wsAudioFrames===0;wsAudio=null;clearWsAudioFallback();if(shouldFallback){setAudio(ch,gain)}};wsAudio.onerror=()=>{}}");
  html += F("function retryPendingAudio(){if(!pendingAudioStart)return;const pending=pendingAudioStart;pendingAudioStart=null;startWsAudio(pending.ch,pending.gain)}");
  html += F("function startPageAudio(ch,gain){startWsAudio(ch,gain,true)}");
  html += F("function startLive(){stream();startPageAudio(liveAudioChannel,liveAudioGain)}");
  html += F("function chooseLiveAudio(ch,gain){if(!img.getAttribute('src'))stream();startPageAudio(ch,gain)}");
  html += F("function teardownTalkback(closeSocket,disableSpeaker){const hadTalkback=!!(talkbackWs||talkbackStream||talkbackSource||talkbackProcessor);if(talkbackProcessor){talkbackProcessor.disconnect();talkbackProcessor.onaudioprocess=null;talkbackProcessor=null}if(talkbackSource){talkbackSource.disconnect();talkbackSource=null}if(talkbackSink){talkbackSink.disconnect();talkbackSink=null}if(talkbackStream){talkbackStream.getTracks().forEach(t=>t.stop());talkbackStream=null}if(closeSocket&&talkbackWs){const ws=talkbackWs;talkbackWs=null;try{ws.close()}catch(e){}}if(disableSpeaker&&hadTalkback){fetch('/speaker/disable').catch(()=>{})}return hadTalkback}");
  html += F("function stopTalkback(){if(teardownTalkback(true,false)){setTimeout(()=>fetch('/speaker/disable').catch(()=>{}),150)}}");
  html += F("async function startTalkback(gain){const ctx=ensureAudioContext();if(!ctx)return;stopTalkback();try{await ctx.resume()}catch(e){}try{talkbackStream=await requestBrowserMicrophone()}catch(e){if(e&&e.message==='secure-context-required'){alert('Browser microphone on phone/Chrome requires HTTPS or localhost. Plain http://192.168.x.x pages cannot capture the browser microphone.')}else{alert('Microphone permission denied or unavailable in this browser.')}return}talkbackWs=new WebSocket(speakerWsUrl(gain));talkbackWs.binaryType='arraybuffer';talkbackWs.onopen=()=>{talkbackSource=ctx.createMediaStreamSource(talkbackStream);talkbackProcessor=ctx.createScriptProcessor(1024,1,1);talkbackSink=ctx.createGain();talkbackSink.gain.value=0;talkbackProcessor.onaudioprocess=event=>{if(!talkbackWs||talkbackWs.readyState!==WebSocket.OPEN)return;const input=event.inputBuffer.getChannelData(0);const pcm=downsampleToInt16(input,ctx.sampleRate,16000);if(pcm.length>0)talkbackWs.send(pcm.buffer)};talkbackSource.connect(talkbackProcessor);talkbackProcessor.connect(talkbackSink);talkbackSink.connect(ctx.destination)};talkbackWs.onclose=()=>{talkbackWs=null;teardownTalkback(false,false)};talkbackWs.onerror=()=>{teardownTalkback(true,false)}}");
  html += F("function setAudio(ch,gain){stopWsAudio();aud.src=audioUrl(ch,gain);aud.play().catch(()=>{})}");
  html += F("aud.src=audioUrl('left',4);");
  html += F("function speakerTone(){fetch('/speaker/tone?freq=1000&ms=1200&amp=2500').catch(()=>{})}");
  html += F("function micSpeaker(ch){fetch('/speaker/mic?ch='+ch+'&shift=4&gain=6&ms=3000').catch(()=>{})}");
  html += F("function sampleAudio(ch){window.open(audioSampleUrl(ch),'_blank')}");
  html += F("function snapshot(){img.src='/capture?cache='+Date.now();statusSoon()}");
  html += F("function stream(){img.src='/stream?cache='+Date.now();statusSoon()}");
  html += F("function stopView(){stopWsAudio();stopTalkback();img.removeAttribute('src');img.alt='Stopped';fetch('/camera/stop').then(statusSoon).catch(()=>{})}");
  html += F("function statusSoon(){setTimeout(()=>fetch('/health').then(r=>r.json()).then(j=>document.getElementById('state').textContent=j.camera).catch(()=>{}),400)}");
  html += F("window.addEventListener('pointerdown',retryPendingAudio,{passive:true});");
  html += F("window.addEventListener('keydown',retryPendingAudio);");
  html += F("window.addEventListener('load',()=>{");
  if (AUTO_START_AV_ON_PAGE_LOAD) {
    html += F("if(isMobileBrowser()){setTimeout(stream,250)}else{setTimeout(startLive,250)}");
  }
  html += F("});");
  html += F("</script></main></body></html>");

  return html;
}

static bool frameToJpeg(camera_fb_t *fb, uint8_t **jpgBuffer, size_t *jpgLength, bool *mustFree) {
  if (fb->format == PIXFORMAT_JPEG) {
    *jpgBuffer = fb->buf;
    *jpgLength = fb->len;
    *mustFree = false;
    return true;
  }

  *jpgBuffer = nullptr;
  *jpgLength = 0;
  *mustFree = true;
  return frame2jpg(fb, CAMERA_JPEG_QUALITY, jpgBuffer, jpgLength);
}

static void sendJpeg(const uint8_t *jpgBuffer, size_t jpgLength) {
  WiFiClient client = server.client();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: image/jpeg");
  client.println("Cache-Control: no-store");
  client.printf("Content-Length: %u\r\n\r\n", static_cast<unsigned>(jpgLength));
  client.write(jpgBuffer, jpgLength);
}

static void handleRoot() {
  Serial.println("HTTP /");
  if (!requireAuth()) {
    return;
  }

  server.send(200, "text/html; charset=utf-8", htmlPage());
}

static void handleHealth() {
  if (!requireAuth()) {
    return;
  }

  String json;
  json.reserve(520);
  json += "{";
  json += "\"camera\":\"" + cameraStatus + "\",";
  json += "\"camera_ready\":" + String(cameraReady ? "true" : "false") + ",";
  json += "\"microphone\":\"" + microphoneStatus + "\",";
  json += "\"microphone_ready\":" + String(microphoneReady ? "true" : "false") + ",";
  json += "\"speaker\":\"" + speakerStatus + "\",";
  json += "\"speaker_ready\":" + String(speakerReady ? "true" : "false") + ",";
  json += "\"relay\":\"" + relayStatus + "\",";
  json += "\"relay_ready\":" + String(relayConfigured() ? "true" : "false") + ",";
  json += "\"relay_control_connected\":" + String(relayControlConnected ? "true" : "false") + ",";
  json += "\"relay_media_connected\":" + String(relayMediaConnected ? "true" : "false") + ",";
  json += "\"relay_speaker_connected\":" + String(relaySpeakerConnected ? "true" : "false") + ",";
  json += "\"mqtt\":\"" + mqttStatus + "\",";
  json += "\"mqtt_ready\":" + String(mqttBridgeConfigured() ? "true" : "false") + ",";
  json += "\"mqtt_connected\":" + String(mqttConnected ? "true" : "false") + ",";
  json += "\"audio_url\":\"http://";
  String audioHost = currentStaIp() == "not connected" ? currentApIp() : currentStaIp();
  json += audioHost;
  json += ":" + String(AUDIO_PORT) + "/audio.wav\",";
  json += "\"audio_sample_url\":\"http://";
  json += audioHost;
  json += ":" + String(AUDIO_PORT) + "/audio/sample.wav\",";
  json += "\"speaker_ws_url\":\"ws://";
  json += audioHost;
  json += ":" + String(SPEAKER_INPUT_PORT) + "/speaker.ws\",";
  json += "\"relay_host\":\"" + String(RELAY_HOST) + "\",";
  json += "\"relay_device_id\":\"" + String(RELAY_DEVICE_ID) + "\",";
  json += "\"mqtt_host\":\"" + String(MQTT_BROKER_HOST) + "\",";
  json += "\"mqtt_device_id\":\"" + String(MQTT_DEVICE_ID) + "\",";
  json += "\"sta_ip\":\"" + currentStaIp() + "\",";
  json += "\"ap_ip\":\"" + currentApIp() + "\",";
  json += "\"psram\":" + String(hasPsramHeap() ? "true" : "false") + ",";
  json += "\"psram_free\":" + String(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) + ",";
  json += "\"pinset\":\"" + String(CAMERA_PINSET_NAME) + "\",";
  json += "\"frame\":\"QVGA JPEG, 1 fb\"";
  json += "}";

  server.send(200, "application/json", json);
}

static void handlePins() {
  if (!requireAuth()) {
    return;
  }

  String text;
  text.reserve(512);
  text += "Pinset: " CAMERA_PINSET_NAME "\n";
  text += "PWDN=" + String(CAMERA_PIN_PWDN) + "\n";
  text += "RESET=" + String(CAMERA_PIN_RESET) + "\n";
  text += "XCLK=" + String(CAMERA_PIN_XCLK) + "\n";
  text += "SIOD=" + String(CAMERA_PIN_SIOD) + "\n";
  text += "SIOC=" + String(CAMERA_PIN_SIOC) + "\n";
  text += "Y2=" + String(CAMERA_PIN_Y2) + "\n";
  text += "Y3=" + String(CAMERA_PIN_Y3) + "\n";
  text += "Y4=" + String(CAMERA_PIN_Y4) + "\n";
  text += "Y5=" + String(CAMERA_PIN_Y5) + "\n";
  text += "Y6=" + String(CAMERA_PIN_Y6) + "\n";
  text += "Y7=" + String(CAMERA_PIN_Y7) + "\n";
  text += "Y8=" + String(CAMERA_PIN_Y8) + "\n";
  text += "Y9=" + String(CAMERA_PIN_Y9) + "\n";
  text += "VSYNC=" + String(CAMERA_PIN_VSYNC) + "\n";
  text += "HREF=" + String(CAMERA_PIN_HREF) + "\n";
  text += "PCLK=" + String(CAMERA_PIN_PCLK) + "\n";
  text += "\n";
  text += "Microphone pinset: " MIC_PINSET_NAME "\n";
  text += "MIC_PDM_DATA=" + String(MIC_PIN_DATA) + "\n";
  text += "MIC_PDM_CLK=" + String(MIC_PIN_CLK) + "\n";
  text += "MIC_LR_SELECT=" + String(MIC_PIN_LR) + "\n";
  text += "\n";
  text += "Speaker pinset: " SPEAKER_PINSET_NAME "\n";
  text += "SPK_PA_CTRL=" + String(SPEAKER_PIN_PA_CTRL) + "\n";
  text += "SPK_LRCLK=" + String(SPEAKER_PIN_LRCLK) + "\n";
  text += "SPK_BCLK=" + String(SPEAKER_PIN_BCLK) + "\n";
  text += "SPK_DOUT=" + String(SPEAKER_PIN_DOUT) + "\n";

  server.send(200, "text/plain; charset=utf-8", text);
}

static void handleCapture() {
  uint32_t startedAt = millis();
  Serial.println("HTTP /capture");

  if (!requireAuth()) {
    return;
  }

  if (!initCamera()) {
    server.send(503, "text/plain; charset=utf-8", cameraStatus);
    return;
  }
  applyCameraSensorProfile(CAMERA_FRAME_SIZE, CAMERA_JPEG_QUALITY, false);

  camera_fb_t *fb = esp_camera_fb_get();
  lastCameraUseMs = millis();
  if (fb == nullptr) {
    server.send(500, "text/plain; charset=utf-8", "Camera capture failed");
    return;
  }

  uint8_t *jpgBuffer = nullptr;
  size_t jpgLength = 0;
  bool mustFree = false;

  if (!frameToJpeg(fb, &jpgBuffer, &jpgLength, &mustFree)) {
    esp_camera_fb_return(fb);
    server.send(500, "text/plain; charset=utf-8", "JPEG conversion failed");
    return;
  }

  sendJpeg(jpgBuffer, jpgLength);

  if (mustFree) {
    free(jpgBuffer);
  }
  esp_camera_fb_return(fb);

  Serial.printf("Capture served in %lu ms, jpg=%u bytes\n",
                static_cast<unsigned long>(millis() - startedAt),
                static_cast<unsigned>(jpgLength));
}

static void handleStream() {
  Serial.println("HTTP /stream");

  if (!requireAuth()) {
    return;
  }

  if (!initCamera()) {
    server.send(503, "text/plain; charset=utf-8", cameraStatus);
    return;
  }
  applyCameraSensorProfile(CAMERA_FRAME_SIZE, CAMERA_JPEG_QUALITY, false);

  WiFiClient client = server.client();
  client.setNoDelay(true);
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Cache-Control: no-store");
  client.println();

  while (client.connected()) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
      Serial.println("Stream frame capture failed");
      break;
    }

    uint8_t *jpgBuffer = nullptr;
    size_t jpgLength = 0;
    bool mustFree = false;

    if (!frameToJpeg(fb, &jpgBuffer, &jpgLength, &mustFree)) {
      esp_camera_fb_return(fb);
      Serial.println("Stream JPEG conversion failed");
      break;
    }

    client.println("--frame");
    client.println("Content-Type: image/jpeg");
    client.printf("Content-Length: %u\r\n\r\n", static_cast<unsigned>(jpgLength));
    client.write(jpgBuffer, jpgLength);
    client.println();

    if (mustFree) {
      free(jpgBuffer);
    }
    esp_camera_fb_return(fb);
    lastCameraUseMs = millis();

    delay(STREAM_DELAY_MS);
    yield();
  }

  stopCamera();
}

static void handleCameraStop() {
  Serial.println("HTTP /camera/stop");
  if (!requireAuth()) {
    return;
  }

  stopCamera();
  server.send(200, "text/plain; charset=utf-8", "Camera stopped");
}

static void playSpeakerTone(uint32_t durationMs, uint16_t frequencyHz, int amplitude) {
  if (!initSpeaker()) {
    return;
  }

  int16_t stereoSamples[SPEAKER_FRAMES * 2];
  float phase = 0.0f;
  float phaseStep = 6.28318530718f * static_cast<float>(frequencyHz) / static_cast<float>(SPEAKER_SAMPLE_RATE);
  unsigned long startedAt = millis();

  while (millis() - startedAt < durationMs) {
    for (size_t frame = 0; frame < SPEAKER_FRAMES; frame++) {
      int16_t sample = static_cast<int16_t>(sinf(phase) * amplitude);
      phase += phaseStep;
      if (phase >= 6.28318530718f) {
        phase -= 6.28318530718f;
      }

      stereoSamples[frame * 2] = sample;
      stereoSamples[frame * 2 + 1] = sample;
    }

    size_t bytesWritten = 0;
    i2s_write(SPEAKER_I2S_PORT,
              stereoSamples,
              sizeof(stereoSamples),
              &bytesWritten,
              pdMS_TO_TICKS(200));
    yield();
  }

  speakerSilence(512);
}

static void handleSpeakerTone() {
  Serial.println("HTTP /speaker/tone");
  if (!requireAuth()) {
    return;
  }

  if (!claimSpeaker(100)) {
    server.send(409, "text/plain; charset=utf-8", "Speaker busy");
    return;
  }

  uint16_t frequencyHz = static_cast<uint16_t>(queryInt(server, "freq", 1000, 100, 4000));
  uint32_t durationMs = static_cast<uint32_t>(queryInt(server, "ms", 1200, 200, 5000));
  int amplitude = queryInt(server, "amp", SPEAKER_DEFAULT_AMPLITUDE, 200, 12000);

  String response = "Playing speaker tone: ";
  response += String(frequencyHz) + " Hz, " + String(durationMs) + " ms, amp=" + String(amplitude);
  server.send(200, "text/plain; charset=utf-8", response);

  playSpeakerTone(durationMs, frequencyHz, amplitude);
  releaseSpeaker();
}

static void handleSpeakerMic() {
  Serial.println("HTTP /speaker/mic");
  if (!requireAuth()) {
    return;
  }

  if (!claimSpeaker(100)) {
    server.send(409, "text/plain; charset=utf-8", "Speaker busy");
    return;
  }

  bool useRightChannel = server.hasArg("ch") && server.arg("ch") == "right";
  int sampleShift = queryInt(server, "shift", 6, 2, 10);
  int sampleGain = queryInt(server, "gain", AUDIO_SAMPLE_GAIN, 1, 16);
  uint32_t durationMs = static_cast<uint32_t>(queryInt(server, "ms", 3000, 500, 8000));

  if (!initMicrophone()) {
    releaseSpeaker();
    server.send(503, "text/plain; charset=utf-8", microphoneStatus);
    return;
  }
  if (!initSpeaker()) {
    releaseSpeaker();
    server.send(503, "text/plain; charset=utf-8", speakerStatus);
    return;
  }

  String response = "Playing microphone through speaker quietly: ";
  response += useRightChannel ? "right" : "left";
  response += ", shift=" + String(sampleShift) + ", gain=" + String(sampleGain) + ", ms=" + String(durationMs);
  server.send(200, "text/plain; charset=utf-8", response);

  int16_t rawSamples[AUDIO_FRAMES];
  int16_t monoSamples[AUDIO_FRAMES];
  int16_t stereoSamples[AUDIO_FRAMES * 2];
  int32_t dcEstimate = 0;
  unsigned long startedAt = millis();

  while (millis() - startedAt < durationMs) {
    size_t bytesRead = 0;
    esp_err_t err = readMicrophoneSamples(rawSamples,
                                          AUDIO_FRAMES,
                                          &bytesRead,
                                          1000);
    if (err != ESP_OK || bytesRead == 0) {
      delay(2);
      continue;
    }

    size_t frameCount = bytesRead / sizeof(rawSamples[0]);
    if (frameCount > AUDIO_FRAMES) {
      frameCount = AUDIO_FRAMES;
    }

    convertMicFramesToPcm(rawSamples, frameCount, useRightChannel, sampleShift, sampleGain, dcEstimate, monoSamples);
    for (size_t frame = 0; frame < frameCount; frame++) {
      stereoSamples[frame * 2] = monoSamples[frame];
      stereoSamples[frame * 2 + 1] = monoSamples[frame];
    }

    size_t bytesWritten = 0;
    i2s_write(SPEAKER_I2S_PORT,
              stereoSamples,
              frameCount * 2 * sizeof(stereoSamples[0]),
              &bytesWritten,
              pdMS_TO_TICKS(200));
    yield();
  }

  speakerSilence(512);
  releaseSpeaker();
}

static void handleSpeakerDisable() {
  Serial.println("HTTP /speaker/disable");
  if (!requireAuth()) {
    return;
  }

  if (!claimSpeaker(100)) {
    server.send(409, "text/plain; charset=utf-8", "Speaker busy");
    return;
  }

  if (speakerReady) {
    speakerSilence(512);
    stopSpeakerChannelHardware();
  }
  pinMode(SPEAKER_PIN_PA_CTRL, OUTPUT);
  digitalWrite(SPEAKER_PIN_PA_CTRL, LOW);
  speakerStatus = "standby";
  server.send(200, "text/plain; charset=utf-8", "Speaker disabled");
  releaseSpeaker();
}

static void handleNotFound() {
  if (!requireAuth()) {
    return;
  }

  server.send(404, "text/plain; charset=utf-8", "Not found");
}

static bool relaySendText(WebSocketsClient &socket, const String &line) {
  if (!claimRelaySocket(200)) {
    return false;
  }
  String payload = line;
  bool ok = socket.sendTXT(payload);
  releaseRelaySocket();
  return ok;
}

static bool relaySendMediaPacket(uint8_t packetType, const uint8_t *payload, size_t payloadLength) {
  if (!relayMediaConnected || payload == nullptr || payloadLength == 0) {
    return false;
  }

  size_t packetLength = payloadLength + 1;
  uint8_t stackPacket[1 + AUDIO_FRAMES * sizeof(int16_t)];
  uint8_t *packet = stackPacket;
  bool mustFree = false;
  if (packetLength > sizeof(stackPacket)) {
    packet = static_cast<uint8_t *>(heap_caps_malloc(packetLength, hasPsramHeap() ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : MALLOC_CAP_8BIT));
    if (packet == nullptr) {
      return false;
    }
    mustFree = true;
  }

  packet[0] = packetType;
  memcpy(packet + 1, payload, payloadLength);

  bool ok = false;
  if (claimRelaySocket(500)) {
    ok = relayMediaSocket.sendBIN(packet, packetLength);
    releaseRelaySocket();
  }

  if (mustFree) {
    free(packet);
  }
  return ok;
}

static bool relayWriteSpeakerPcmBytes(const uint8_t *payload, size_t payloadLength) {
  if (payload == nullptr || payloadLength == 0 || (payloadLength % sizeof(int16_t)) != 0) {
    return false;
  }

  int16_t monoSamples[SPEAKER_FRAMES * 4];
  size_t offset = 0;
  while (offset < payloadLength) {
    size_t chunkBytes = min(payloadLength - offset, sizeof(monoSamples));
    if ((chunkBytes % sizeof(int16_t)) != 0) {
      chunkBytes--;
    }
    size_t frameCount = chunkBytes / sizeof(int16_t);
    if (frameCount == 0) {
      break;
    }

    for (size_t i = 0; i < frameCount; i++) {
      uint16_t lo = payload[offset + i * 2];
      uint16_t hi = payload[offset + i * 2 + 1];
      monoSamples[i] = static_cast<int16_t>((hi << 8) | lo);
    }

    if (!writeSpeakerMonoPcm(monoSamples, frameCount, RELAY_INTERCOM_PLAYBACK_GAIN)) {
      return false;
    }
    offset += chunkBytes;
  }

  return true;
}

static void relaySendStateLine() {
  if (!relayConfigured() || !relayControlConnected) {
    return;
  }

  String line;
  line.reserve(256);
  line += "state";
  line += " camera=" + relaySafeValue(cameraStatus);
  line += " mic=" + relaySafeValue(microphoneStatus);
  line += " speaker=" + relaySafeValue(speakerStatus);
  line += " heap=" + String(ESP.getFreeHeap());
  line += " sta=" + relaySafeValue(currentStaIp());
  line += " ap=" + relaySafeValue(currentApIp());
  line += " video=" + String(relayVideoRequested ? 1 : 0);
  line += " audio=" + String(relayAudioRequested ? 1 : 0);
  line += " audio_ch=" + String(relayUseRightChannel ? "right" : "left");
  line += " audio_gain=" + String(relayAudioGain);
  line += " audio_shift=" + String(relayAudioShift);
  line += " local_http=" + relaySafeValue(String("http://") + (currentStaIp() == "not connected" ? currentApIp() : currentStaIp()));
  relaySendText(relayControlSocket, line);
}

static void relayApplySubscription(const String &line) {
  if (!line.startsWith("sub ")) {
    return;
  }

  relayVideoRequested = relayFieldBool(line, "video", relayVideoRequested);
  relayAudioRequested = relayFieldBool(line, "audio", relayAudioRequested);
  relayUseRightChannel = relayFieldValue(line, "ch", relayUseRightChannel ? "right" : "left") == "right";
  relayAudioGain = relayFieldInt(line, "gain", relayAudioGain, 1, 16);
  relayAudioShift = relayFieldInt(line, "shift", relayAudioShift, 0, 8);
  relayStatus = "cloud_subscribed";
  relaySendStateLine();
}

static void relayControlSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      relayControlConnected = true;
      relayStatus = "cloud_control_connected";
      Serial.println("Relay control connected");
      relaySendText(relayControlSocket, String("hello device=") + RELAY_DEVICE_ID + " fw=esp32-s3-cam-den");
      relaySendStateLine();
      break;
    case WStype_DISCONNECTED:
      relayControlConnected = false;
      relayStatus = "cloud_control_disconnected";
      relayVideoRequested = false;
      relayAudioRequested = false;
      Serial.println("Relay control disconnected");
      break;
    case WStype_TEXT: {
      String line(reinterpret_cast<char *>(payload), length);
      line.trim();
      Serial.print("Relay control <- ");
      Serial.println(line);
      if (line.startsWith("sub ")) {
        relayApplySubscription(line);
      } else if (line == "ping") {
        relaySendText(relayControlSocket, "pong");
      }
      break;
    }
    case WStype_ERROR:
      relayStatus = "cloud_control_error";
      if (payload != nullptr && length > 0) {
        Serial.print("Relay control error: ");
        Serial.write(payload, length);
        Serial.println();
      } else {
        Serial.println("Relay control error");
      }
      break;
    default:
      break;
  }
}

static void relayMediaSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      relayMediaConnected = true;
      relayStatus = "cloud_media_connected";
      Serial.println("Relay media connected");
      break;
    case WStype_DISCONNECTED:
      relayMediaConnected = false;
      relayStatus = "cloud_media_disconnected";
      Serial.println("Relay media disconnected");
      break;
    case WStype_ERROR:
      relayStatus = "cloud_media_error";
      if (payload != nullptr && length > 0) {
        Serial.print("Relay media error: ");
        Serial.write(payload, length);
        Serial.println();
      } else {
        Serial.println("Relay media error");
      }
      break;
    case WStype_BIN:
      if (payload == nullptr || length < 2) {
        break;
      }
      if (payload[0] != RELAY_PACKET_AUDIO_PCM) {
        break;
      }
      if (!claimSpeaker(10)) {
        return;
      }
      if (!initSpeaker()) {
        releaseSpeaker();
        return;
      }
      speakerStatus = "relay_peer_audio";
      relayWriteSpeakerPcmBytes(payload + 1, length - 1);
      lastRelaySpeakerChunkMs = millis();
      releaseSpeaker();
      break;
    default:
      break;
  }
}

static void relaySpeakerSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      relaySpeakerConnected = true;
      relayStatus = "cloud_speaker_connected";
      Serial.println("Relay speaker connected");
      break;
    case WStype_DISCONNECTED:
      relaySpeakerConnected = false;
      relayStatus = "cloud_speaker_disconnected";
      Serial.println("Relay speaker disconnected");
      break;
    case WStype_BIN:
      if (!claimSpeaker(10)) {
        return;
      }
      if (!initSpeaker()) {
        releaseSpeaker();
        return;
      }
      speakerStatus = "relay_talkback";
      relayWriteSpeakerPcmBytes(payload, length);
      lastRelaySpeakerChunkMs = millis();
      releaseSpeaker();
      break;
    case WStype_ERROR:
      relayStatus = "cloud_speaker_error";
      if (payload != nullptr && length > 0) {
        Serial.print("Relay speaker error: ");
        Serial.write(payload, length);
        Serial.println();
      } else {
        Serial.println("Relay speaker error");
      }
      break;
    default:
      break;
  }
}

static void relayMediaTask(void *parameter) {
  (void)parameter;

  int16_t rawSamples[AUDIO_FRAMES];
  int16_t pcmSamples[AUDIO_FRAMES];
  int32_t dcEstimate = 0;
  unsigned long lastVideoAt = 0;

  for (;;) {
    bool didWork = false;
    bool videoRequested = hybridCloudMode() ? mqttVideoRequested : relayVideoRequested;
    bool audioRequested = hybridCloudMode() ? mqttAudioRequested : relayAudioRequested;
    if (relayIntercomEnabled() && !hybridCloudMode()) {
      audioRequested = true;
    }
    bool useRightChannel = hybridCloudMode() ? mqttUseRightChannel : relayUseRightChannel;
    int audioShift = hybridCloudMode() ? mqttAudioShift : relayAudioShift;
    int audioGain = hybridCloudMode() ? mqttAudioGain : relayAudioGain;
    if (relayIntercomEnabled() && !hybridCloudMode()) {
      audioGain = max(audioGain, RELAY_INTERCOM_CAPTURE_GAIN);
    }
    bool talkbackActive = relayTalkbackActive() || mqttTalkbackActive();

    if (relayConfigured() && relayMediaConnected && audioRequested && !talkbackActive) {
      if (microphoneReady || initMicrophone()) {
        size_t bytesRead = 0;
        esp_err_t err = readMicrophoneSamples(rawSamples, AUDIO_FRAMES, &bytesRead, 60);
        if (err == ESP_OK && bytesRead > 0) {
          size_t frameCount = bytesRead / sizeof(rawSamples[0]);
          if (frameCount > AUDIO_FRAMES) {
            frameCount = AUDIO_FRAMES;
          }

          convertMicFramesToPcm(rawSamples,
                                frameCount,
                                useRightChannel,
                                audioShift,
                                audioGain,
                                dcEstimate,
                                pcmSamples);

          if (frameCount > 0) {
            relaySendMediaPacket(RELAY_PACKET_AUDIO_PCM,
                                 reinterpret_cast<uint8_t *>(pcmSamples),
                                 frameCount * sizeof(pcmSamples[0]));
            didWork = true;
          }
        }
      }
    }

    unsigned long now = millis();
    if (relayConfigured() &&
        relayMediaConnected &&
        videoRequested &&
        !talkbackActive &&
        now - lastVideoAt >= RELAY_FRAME_INTERVAL_MS) {
      if (initCamera()) {
        if (audioRequested) {
          applyCameraSensorProfile(RELAY_AV_CAMERA_FRAME_SIZE, RELAY_AV_CAMERA_JPEG_QUALITY, false);
        } else {
          applyCameraSensorProfile(RELAY_VIDEO_ONLY_CAMERA_FRAME_SIZE, RELAY_VIDEO_ONLY_CAMERA_JPEG_QUALITY, false);
        }
        camera_fb_t *fb = esp_camera_fb_get();
        lastCameraUseMs = millis();
        if (fb != nullptr) {
          uint8_t *jpgBuffer = nullptr;
          size_t jpgLength = 0;
          bool mustFree = false;
          if (frameToJpeg(fb, &jpgBuffer, &jpgLength, &mustFree) && jpgLength > 0) {
            relaySendMediaPacket(RELAY_PACKET_VIDEO_JPEG, jpgBuffer, jpgLength);
            didWork = true;
          }
          if (mustFree) {
            free(jpgBuffer);
          }
          esp_camera_fb_return(fb);
        }
      }
      lastVideoAt = now;
    }

    if (!didWork) {
      vTaskDelay(pdMS_TO_TICKS(audioRequested ? 4 : 20));
    } else {
      taskYIELD();
    }
  }
}

static void startRelayClients() {
  if (!relayConfigured()) {
    Serial.println("Cloud relay disabled");
    relayStatus = "disabled";
    return;
  }

  relayMediaPath = relayBuildPath("media");
  relaySpeakerPath = relayBuildPath("speaker");
  relayControlPath = hybridCloudMode() ? "" : relayBuildPath("control");
  relaySocketMutex = xSemaphoreCreateRecursiveMutex();
  const bool speakerChannelEnabled = relaySpeakerChannelEnabled();

  relayMediaSocket.onEvent(relayMediaSocketEvent);
  if (speakerChannelEnabled) {
    relaySpeakerSocket.onEvent(relaySpeakerSocketEvent);
  }
  if (!hybridCloudMode()) {
    relayControlSocket.onEvent(relayControlSocketEvent);
  }

  if (RELAY_USE_TLS) {
    relayMediaSocket.beginSSL(RELAY_HOST, RELAY_PORT, relayMediaPath.c_str());
    if (speakerChannelEnabled) {
      relaySpeakerSocket.beginSSL(RELAY_HOST, RELAY_PORT, relaySpeakerPath.c_str());
    }
    if (!hybridCloudMode()) {
      relayControlSocket.beginSSL(RELAY_HOST, RELAY_PORT, relayControlPath.c_str());
    }
  } else {
    relayMediaSocket.begin(RELAY_HOST, RELAY_PORT, relayMediaPath.c_str());
    if (speakerChannelEnabled) {
      relaySpeakerSocket.begin(RELAY_HOST, RELAY_PORT, relaySpeakerPath.c_str());
    }
    if (!hybridCloudMode()) {
      relayControlSocket.begin(RELAY_HOST, RELAY_PORT, relayControlPath.c_str());
    }
  }

  relayMediaSocket.setReconnectInterval(RELAY_RECONNECT_INTERVAL_MS);
  relayMediaSocket.enableHeartbeat(15000, 3000, 2);
  if (speakerChannelEnabled) {
    relaySpeakerSocket.setReconnectInterval(RELAY_RECONNECT_INTERVAL_MS);
    relaySpeakerSocket.enableHeartbeat(15000, 3000, 2);
  } else {
    relaySpeakerConnected = false;
    Serial.println("Cloud speaker relay skipped in hybrid mode");
  }
  if (!hybridCloudMode()) {
    relayControlSocket.setReconnectInterval(RELAY_RECONNECT_INTERVAL_MS);
    relayControlSocket.enableHeartbeat(15000, 3000, 2);
  }

  BaseType_t taskStarted = xTaskCreatePinnedToCore(
      relayMediaTask,
      "relay_media",
      12288,
      nullptr,
      1,
      &relayMediaTaskHandle,
      1);

  Serial.print(hybridCloudMode() ? "Cloud media relay configured: " : "Cloud relay configured: ");
  Serial.println(RELAY_HOST);
  if (taskStarted != pdPASS) {
    relayStatus = "cloud_task_failed";
    Serial.println("Relay media task failed");
  }
}

static String mqttTopicFor(const char *suffix) {
  String topic = mqttTopicPrefix;
  if (!topic.endsWith("/")) {
    topic += "/";
  }
  topic += suffix;
  return topic;
}

static bool mqttPublishText(const String &topic, const String &payload, bool retained) {
  if (!mqttConnected) {
    return false;
  }
  if (!claimMqttClient(500)) {
    return false;
  }
  bool ok = mqttClient.publish(topic.c_str(), payload.c_str(), retained);
  if (!ok) {
    markMqttDisconnected("mqtt_publish_failed");
  }
  releaseMqttClient();
  return ok;
}

static bool mqttPublishBinary(const String &topic, const uint8_t *payload, size_t payloadLength) {
  if (!mqttConnected || payload == nullptr || payloadLength == 0) {
    return false;
  }
  if (!claimMqttClient(1000)) {
    return false;
  }

  bool ok = mqttClient.beginPublish(topic.c_str(), payloadLength, false);
  if (ok) {
    ok = mqttClient.write(payload, payloadLength) == payloadLength;
  }
  if (ok) {
    ok = mqttClient.endPublish();
  } else {
    markMqttDisconnected("mqtt_publish_failed");
  }
  releaseMqttClient();
  return ok;
}

static void markMqttDisconnected(const char *status) {
  mqttClient.disconnect();
  mqttTransport.stop();
  mqttConnected = false;
  mqttStatus = status;
}

static bool mqttTalkbackActive() {
  return speakerStatus == "mqtt_talkback" &&
         lastMqttSpeakerChunkMs > 0 &&
         millis() - lastMqttSpeakerChunkMs < 1500;
}

static bool relayTalkbackActive() {
  return speakerStatus == "relay_talkback" &&
         lastRelaySpeakerChunkMs > 0 &&
         millis() - lastRelaySpeakerChunkMs < 1500;
}

static bool mqttHasBrokerCredentials() {
  return strlen(MQTT_USERNAME) > 0;
}

static void mqttSendStateLine() {
  if (!mqttBridgeConfigured() || !mqttConnected) {
    return;
  }

  String line;
  line.reserve(256);
  line += "state";
  line += " camera=" + relaySafeValue(cameraStatus);
  line += " mic=" + relaySafeValue(microphoneStatus);
  line += " speaker=" + relaySafeValue(speakerStatus);
  line += " heap=" + String(ESP.getFreeHeap());
  line += " sta=" + relaySafeValue(currentStaIp());
  line += " ap=" + relaySafeValue(currentApIp());
  line += " video=" + String(mqttVideoRequested ? 1 : 0);
  line += " audio=" + String(mqttAudioRequested ? 1 : 0);
  line += " audio_ch=" + String(mqttUseRightChannel ? "right" : "left");
  line += " audio_gain=" + String(mqttAudioGain);
  line += " audio_shift=" + String(mqttAudioShift);
  line += " broker=" + relaySafeValue(MQTT_BROKER_HOST);
  mqttPublishText(mqttStateTopic, line, true);
}

static void mqttApplySubscription(const String &line) {
  if (!line.startsWith("sub ")) {
    return;
  }

  mqttVideoRequested = relayFieldBool(line, "video", mqttVideoRequested);
  mqttAudioRequested = relayFieldBool(line, "audio", mqttAudioRequested);
  mqttUseRightChannel = relayFieldValue(line, "ch", mqttUseRightChannel ? "right" : "left") == "right";
  mqttAudioGain = relayFieldInt(line, "gain", mqttAudioGain, 1, 16);
  mqttAudioShift = relayFieldInt(line, "shift", mqttAudioShift, 0, 8);
  mqttStatus = "mqtt_subscribed";
  mqttSendStateLine();
}

static void mqttCallback(char *topic, uint8_t *payload, unsigned int length) {
  String topicName = topic == nullptr ? "" : String(topic);
  if (topicName == mqttControlTopic) {
    String line;
    line.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) {
      line += static_cast<char>(payload[i]);
    }
    line.trim();
    Serial.print("MQTT control <- ");
    Serial.println(line);
    mqttApplySubscription(line);
    return;
  }

  if (topicName != mqttSpeakerTopic || length == 0) {
    return;
  }

  if (!claimSpeaker(20)) {
    return;
  }
  if (!initSpeaker()) {
    releaseSpeaker();
    return;
  }
  speakerStatus = "mqtt_talkback";
  relayWriteSpeakerPcmBytes(payload, length);
  lastMqttSpeakerChunkMs = millis();
  releaseSpeaker();
}

static bool connectMqttBridge() {
  if (!mqttBridgeConfigured()) {
    mqttStatus = "disabled";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    mqttStatus = "wifi_required";
    return false;
  }
  if (mqttConnected) {
    return true;
  }

  unsigned long now = millis();
  if (lastMqttConnectAttemptMs > 0 && now - lastMqttConnectAttemptMs < MQTT_RECONNECT_INTERVAL_MS) {
    return false;
  }
  lastMqttConnectAttemptMs = now;

  if (!claimMqttClient(1000)) {
    return false;
  }

  mqttTransport.stop();
#if MQTT_USE_TLS && MQTT_ALLOW_INSECURE_TLS
  mqttTransport.setInsecure();
#endif
  mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(8);
  mqttClient.setBufferSize(32768);

  uint64_t chipId = ESP.getEfuseMac();
  String clientId = String(MQTT_DEVICE_ID) + "-" + String(static_cast<uint32_t>(chipId & 0xffffffff), HEX);
  bool connected = false;
  if (mqttHasBrokerCredentials()) {
    connected = mqttClient.connect(clientId.c_str(),
                                   MQTT_USERNAME,
                                   MQTT_PASSWORD,
                                   mqttPresenceTopic.c_str(),
                                   0,
                                   true,
                                   "0");
  } else {
    connected = mqttClient.connect(clientId.c_str(), mqttPresenceTopic.c_str(), 0, true, "0");
  }
  if (!connected) {
    mqttStatus = "mqtt_connect_failed";
    mqttTransport.stop();
    releaseMqttClient();
    return false;
  }

  mqttConnected = true;
  mqttStatus = "mqtt_connected";
  mqttClient.subscribe(mqttControlTopic.c_str(), 0);
  if (!relayConfigured()) {
    mqttClient.subscribe(mqttSpeakerTopic.c_str(), 0);
  }
  mqttClient.publish(mqttPresenceTopic.c_str(), "1", true);
  releaseMqttClient();
  mqttSendStateLine();
  Serial.print("MQTT bridge connected: ");
  Serial.println(MQTT_BROKER_HOST);
  return true;
}

static void mqttMediaTask(void *parameter) {
  (void)parameter;

  int16_t rawSamples[MQTT_AUDIO_FRAMES];
  int16_t pcmSamples[MQTT_AUDIO_FRAMES];
  int32_t dcEstimate = 0;
  unsigned long lastVideoAt = 0;

  for (;;) {
    bool didWork = false;
    bool talkbackActive = mqttTalkbackActive();

    if (mqttBridgeConfigured() && mqttConnected && mqttAudioRequested && !talkbackActive) {
      if (microphoneReady || initMicrophone()) {
        size_t bytesRead = 0;
        esp_err_t err = readMicrophoneSamples(rawSamples, MQTT_AUDIO_FRAMES, &bytesRead, 120);
        if (err == ESP_OK && bytesRead > 0) {
          size_t frameCount = bytesRead / sizeof(rawSamples[0]);
          if (frameCount > MQTT_AUDIO_FRAMES) {
            frameCount = MQTT_AUDIO_FRAMES;
          }
          convertMicFramesToPcm(rawSamples,
                                frameCount,
                                mqttUseRightChannel,
                                mqttAudioShift,
                                mqttAudioGain,
                                dcEstimate,
                                pcmSamples);
          if (frameCount > 0) {
            mqttPublishBinary(mqttAudioTopic,
                              reinterpret_cast<uint8_t *>(pcmSamples),
                              frameCount * sizeof(pcmSamples[0]));
            didWork = true;
          }
        }
      }
    }

    unsigned long now = millis();
    uint32_t effectiveVideoInterval = mqttAudioRequested ? max<uint32_t>(MQTT_VIDEO_INTERVAL_MS, 900) : MQTT_VIDEO_INTERVAL_MS;
    if (mqttBridgeConfigured() && mqttConnected && mqttVideoRequested && !talkbackActive && now - lastVideoAt >= effectiveVideoInterval) {
      if (initCamera()) {
        if (mqttAudioRequested) {
          applyCameraSensorProfile(MQTT_AV_CAMERA_FRAME_SIZE, MQTT_AV_CAMERA_JPEG_QUALITY, false);
        } else {
          applyCameraSensorProfile(MQTT_VIDEO_ONLY_CAMERA_FRAME_SIZE, MQTT_VIDEO_ONLY_CAMERA_JPEG_QUALITY, false);
        }
        camera_fb_t *fb = esp_camera_fb_get();
        lastCameraUseMs = millis();
        if (fb != nullptr) {
          uint8_t *jpgBuffer = nullptr;
          size_t jpgLength = 0;
          bool mustFree = false;
          if (frameToJpeg(fb, &jpgBuffer, &jpgLength, &mustFree) && jpgLength > 0) {
            mqttPublishBinary(mqttVideoTopic, jpgBuffer, jpgLength);
            didWork = true;
          }
          if (mustFree) {
            free(jpgBuffer);
          }
          esp_camera_fb_return(fb);
        }
      }
      lastVideoAt = now;
    }

    if (!didWork) {
      vTaskDelay(pdMS_TO_TICKS((mqttConnected && mqttAudioRequested) ? 4 : 25));
    } else {
      taskYIELD();
    }
  }
}

static void startMqttBridge() {
  if (!mqttBridgeConfigured()) {
    mqttStatus = "disabled";
    Serial.println("MQTT bridge disabled");
    return;
  }

  mqttTopicPrefix = String("mokrenkog/esp32-s3-cam-den/") +
                    relaySafeValue(MQTT_DEVICE_ID) +
                    "/" +
                    relaySafeValue(MQTT_SHARED_KEY);
  mqttControlTopic = mqttTopicFor("control");
  mqttVideoTopic = mqttTopicFor("video/jpeg");
  mqttAudioTopic = mqttTopicFor("audio/pcm");
  mqttSpeakerTopic = mqttTopicFor("speaker/pcm");
  mqttStateTopic = mqttTopicFor("state");
  mqttPresenceTopic = mqttTopicFor("presence");
  mqttClientMutex = xSemaphoreCreateRecursiveMutex();

  if (hybridCloudMode()) {
    Serial.println("MQTT control/state mode active; media handled by relay");
    return;
  }

  BaseType_t taskStarted = xTaskCreatePinnedToCore(
      mqttMediaTask,
      "mqtt_media",
      12288,
      nullptr,
      1,
      &mqttMediaTaskHandle,
      1);

  if (taskStarted != pdPASS) {
    mqttStatus = "mqtt_task_failed";
    Serial.println("MQTT bridge task failed");
    return;
  }

  mqttStatus = "mqtt_ready";
  Serial.print("MQTT bridge configured: ");
  Serial.println(MQTT_BROKER_HOST);
}

static void startWiFi() {
  WiFi.mode(ENABLE_DIRECT_AP ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);

  if (ENABLE_DIRECT_AP) {
    bool apStarted = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    Serial.print("Access point: ");
    Serial.println(apStarted ? WIFI_AP_SSID : "failed");
    Serial.print("Direct AP URL: http://");
    Serial.println(WiFi.softAPIP());
  }

  if (strlen(WIFI_STA_SSID) == 0) {
    Serial.println("Router Wi-Fi not configured. Use direct AP.");
    return;
  }

  Serial.print("Connecting to Wi-Fi SSID: ");
  Serial.println(WIFI_STA_SSID);
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASSWORD);

  for (uint8_t i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Router Wi-Fi connected. Open http://");
    Serial.println(WiFi.localIP());
    return;
  }

  Serial.println("Router Wi-Fi connection failed. Direct AP remains active.");
}

static void startServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/pins", HTTP_GET, handlePins);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/camera/stop", HTTP_GET, handleCameraStop);
  server.on("/speaker/tone", HTTP_GET, handleSpeakerTone);
  server.on("/speaker/mic", HTTP_GET, handleSpeakerMic);
  server.on("/speaker/disable", HTTP_GET, handleSpeakerDisable);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.print("HTTP server started on port ");
  Serial.println(HTTP_PORT);
  Serial.println("Camera is kept initialized to avoid Wi-Fi heap fragmentation.");
}

static void startAudioServer() {
  audioServer.begin();
  BaseType_t taskStarted = xTaskCreatePinnedToCore(
      audioServerTask,
      "audio_http",
      AUDIO_SERVER_TASK_STACK,
      nullptr,
      1,
      nullptr,
      1);

  Serial.print("Audio WAV server started on port ");
  Serial.println(AUDIO_PORT);
  if (taskStarted != pdPASS) {
    microphoneStatus = "audio task failed";
    Serial.println("Audio server task failed");
  }
}

static void startSpeakerInputServer() {
  speakerInputServer.begin();
  BaseType_t taskStarted = xTaskCreatePinnedToCore(
      speakerInputServerTask,
      "speaker_ws",
      SPEAKER_INPUT_TASK_STACK,
      nullptr,
      1,
      nullptr,
      1);

  Serial.print("Speaker WebSocket server started on port ");
  Serial.println(SPEAKER_INPUT_PORT);
  if (taskStarted != pdPASS) {
    speakerStatus = "speaker ws task failed";
    Serial.println("Speaker WebSocket server task failed");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-S3-CAM-DEN browser camera");
  Serial.print("Camera pinset: ");
  Serial.println(CAMERA_PINSET_NAME);
  Serial.print("PSRAM: ");
  Serial.println(hasPsramHeap() ? "available" : "not available");
  Serial.printf("Boot heap: internal=%u largest_internal=%u psram=%u largest_psram=%u\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

  if (INIT_CAMERA_BEFORE_WIFI) {
    initCamera();
  }
  speakerUseMutex = xSemaphoreCreateMutex();
  if (speakerUseMutex == nullptr) {
    Serial.println("Speaker mutex create failed");
  }
  printMicrophoneSelfTest("boot before WiFi/camera");
  startWiFi();
  startMqttBridge();
  startServer();
  startAudioServer();
  startSpeakerInputServer();
  startRelayClients();
}

void loop() {
  server.handleClient();

  if (relayConfigured() && claimRelaySocket(20)) {
    if (!hybridCloudMode()) {
      relayControlSocket.loop();
    }
    relayMediaSocket.loop();
    if (relaySpeakerChannelEnabled()) {
      relaySpeakerSocket.loop();
    }
    releaseRelaySocket();
  }

  if (mqttBridgeConfigured()) {
    if (!mqttConnected) {
      connectMqttBridge();
    } else if (claimMqttClient(20)) {
      bool socketConnected = mqttClient.connected();
      bool loopOk = socketConnected ? mqttClient.loop() : false;
      if (!socketConnected || !loopOk) {
        markMqttDisconnected("mqtt_disconnected");
      }
      releaseMqttClient();
    }
  }

  unsigned long now = millis();
  if (!KEEP_CAMERA_INITIALIZED && cameraReady && lastCameraUseMs > 0 && now - lastCameraUseMs >= CAMERA_IDLE_STOP_MS) {
    Serial.println("Camera idle timeout");
    stopCamera();
  }

  if (!hybridCloudMode() && relayConfigured() && relayControlConnected && now - lastRelayStateMs >= RELAY_STATE_INTERVAL_MS) {
    lastRelayStateMs = now;
    relaySendStateLine();
  }

  if (speakerStatus == "relay_talkback" && lastRelaySpeakerChunkMs > 0 && now - lastRelaySpeakerChunkMs >= 800) {
    speakerStatus = speakerReady ? "ok" : "standby";
  }
  if (speakerStatus == "relay_peer_audio" && lastRelaySpeakerChunkMs > 0 && now - lastRelaySpeakerChunkMs >= 800) {
    speakerStatus = speakerReady ? "ok" : "standby";
  }
  if (speakerStatus == "mqtt_talkback" && lastMqttSpeakerChunkMs > 0 && now - lastMqttSpeakerChunkMs >= 800) {
    speakerStatus = speakerReady ? "ok" : "standby";
  }
  if (mqttBridgeConfigured() && mqttConnected && now - lastMqttStateMs >= MQTT_STATE_INTERVAL_MS) {
    lastMqttStateMs = now;
    mqttSendStateLine();
  }

  if (now - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = now;
    Serial.printf("Alive sta=%s ap=%s heap=%u camera=%s mic=%s speaker=%s relay=%s mqtt=%s\n",
                  currentStaIp().c_str(),
                  currentApIp().c_str(),
                  ESP.getFreeHeap(),
                  cameraStatus.c_str(),
                  microphoneStatus.c_str(),
                  speakerStatus.c_str(),
                  relayStatus.c_str(),
                  mqttStatus.c_str());
  }

  delay(2);
}

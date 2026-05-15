#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
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

constexpr uint16_t HTTP_PORT = 80;
constexpr uint16_t AUDIO_PORT = 81;
constexpr bool ENABLE_DIRECT_AP = true;
constexpr bool INIT_CAMERA_BEFORE_WIFI = false;
constexpr bool KEEP_CAMERA_INITIALIZED = false;
constexpr bool AUTO_START_AV_ON_PAGE_LOAD = true;
constexpr framesize_t CAMERA_FRAME_SIZE = FRAMESIZE_QVGA;
constexpr int CAMERA_JPEG_QUALITY = 14;
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

WebServer server(HTTP_PORT);
WiFiServer audioServer(AUDIO_PORT);

SemaphoreHandle_t speakerUseMutex = nullptr;

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
framesize_t activeCameraFrameSize = CAMERA_FRAME_SIZE;
int activeCameraJpegQuality = CAMERA_JPEG_QUALITY;

static void stopCamera();
static void applyCameraSensorProfile(framesize_t frameSize, int jpegQuality, bool fullTuning = false);

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
  html.reserve(1200);

  html += F("<!doctype html><html><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>ESP32-S3-CAM-DEN</title>");
  html += F("<style>");
  html += F("html,body{margin:0;width:100%;height:100%;background:#000;overflow:hidden}");
  html += F("img{display:block;width:100vw;height:100vh;object-fit:contain;background:#000}");
  html += F("</style></head><body>");
  html += F("<img src='/stream?cache=");
  html += String(millis());
  html += F("' alt='ESP32-S3-CAM-DEN live video'>");
  html += F("</body></html>");

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
  json += "\"audio_url\":\"http://";
  String audioHost = currentStaIp() == "not connected" ? currentApIp() : currentStaIp();
  json += audioHost;
  json += ":" + String(AUDIO_PORT) + "/audio.wav\",";
  json += "\"audio_sample_url\":\"http://";
  json += audioHost;
  json += ":" + String(AUDIO_PORT) + "/audio/sample.wav\",";
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
  startServer();
  startAudioServer();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (!KEEP_CAMERA_INITIALIZED && cameraReady && lastCameraUseMs > 0 && now - lastCameraUseMs >= CAMERA_IDLE_STOP_MS) {
    Serial.println("Camera idle timeout");
    stopCamera();
  }

  if (now - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = now;
    Serial.printf("Alive sta=%s ap=%s heap=%u camera=%s mic=%s speaker=%s\n",
                  currentStaIp().c_str(),
                  currentApIp().c_str(),
                  ESP.getFreeHeap(),
                  cameraStatus.c_str(),
                  microphoneStatus.c_str(),
                  speakerStatus.c_str());
  }

  delay(2);
}

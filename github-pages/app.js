const SETTINGS_KEY = "esp32-s3-cam-den-mqtt-settings";
const AUDIO_SAMPLE_RATE = 16000;
const AUDIO_LEAD_SECONDS = 0.08;
const AUDIO_MAX_BUFFER_SECONDS = 0.35;
const AUDIO_RESET_SECONDS = 0.14;

const elements = {
  brokerUrl: document.getElementById("broker-url"),
  deviceId: document.getElementById("device-id"),
  sharedKey: document.getElementById("shared-key"),
  saveSettings: document.getElementById("save-settings"),
  connect: document.getElementById("connect-btn"),
  disconnect: document.getElementById("disconnect-btn"),
  notice: document.getElementById("notice"),
  startAv: document.getElementById("start-av"),
  videoOnly: document.getElementById("video-only"),
  audioStop: document.getElementById("audio-stop"),
  audioLeft: document.getElementById("audio-left"),
  audioRight: document.getElementById("audio-right"),
  audioLouder: document.getElementById("audio-louder"),
  audioQuieter: document.getElementById("audio-quieter"),
  talkbackStart: document.getElementById("talkback-start"),
  talkbackLouder: document.getElementById("talkback-louder"),
  talkbackStop: document.getElementById("talkback-stop"),
  connectionPill: document.getElementById("connection-pill"),
  deviceState: document.getElementById("device-state"),
  talkbackState: document.getElementById("talkback-state"),
  video: document.getElementById("video"),
  audio: document.getElementById("audio"),
  log: document.getElementById("log"),
  cameraStatus: document.getElementById("camera-status"),
  micStatus: document.getElementById("mic-status"),
  speakerStatus: document.getElementById("speaker-status"),
  heapStatus: document.getElementById("heap-status"),
  staStatus: document.getElementById("sta-status"),
  apStatus: document.getElementById("ap-status"),
  brokerStatus: document.getElementById("broker-status"),
};

const state = {
  mqtt: null,
  audioCtx: null,
  nextAudioTime: 0,
  imageUrl: null,
  subscription: {
    video: true,
    audio: true,
    channel: "left",
    gain: 4,
    shift: 0,
  },
  talkbackStream: null,
  talkbackSource: null,
  talkbackProcessor: null,
  talkbackSink: null,
  talkbackGain: 2,
  talkbackRestoreSubscription: null,
  topics: null,
  boardPresenceSeen: false,
  boardPresenceTimer: null,
};

function log(message) {
  const stamp = new Date().toLocaleTimeString();
  elements.log.textContent = `[${stamp}] ${message}\n${elements.log.textContent}`.trim();
}

function setNotice(text, kind = "info") {
  elements.notice.textContent = text;
  elements.notice.className = `notice notice-${kind}`;
}

function getSettings() {
  try {
    return JSON.parse(localStorage.getItem(SETTINGS_KEY) || "{}");
  } catch {
    return {};
  }
}

function saveSettings() {
  const settings = {
    brokerUrl: elements.brokerUrl.value.trim(),
    deviceId: elements.deviceId.value.trim(),
    sharedKey: elements.sharedKey.value.trim(),
  };
  localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
  log("Settings saved");
  return settings;
}

function loadSettings() {
  const settings = getSettings();
  elements.brokerUrl.value = settings.brokerUrl || "wss://broker.emqx.io:8084/mqtt";
  elements.deviceId.value = settings.deviceId || "esp32-s3-cam-den-01";
  elements.sharedKey.value = settings.sharedKey || "";
}

function requireSettings() {
  const settings = saveSettings();
  if (!settings.brokerUrl || !settings.deviceId || !settings.sharedKey) {
    setNotice("Fill Broker WSS URL, Device ID and Shared key before connecting.", "error");
    throw new Error("Fill broker URL, device ID and shared key");
  }
  return settings;
}

function topicPrefix(settings) {
  return `mokrenkog/esp32-s3-cam-den/${settings.deviceId}/${settings.sharedKey}`;
}

function buildTopics(settings) {
  const prefix = topicPrefix(settings);
  return {
    control: `${prefix}/control`,
    video: `${prefix}/video/jpeg`,
    audio: `${prefix}/audio/pcm`,
    speaker: `${prefix}/speaker/pcm`,
    state: `${prefix}/state`,
    presence: `${prefix}/presence`,
  };
}

function ensureAudioContext() {
  const AudioContextClass = window.AudioContext || window.webkitAudioContext;
  if (!AudioContextClass) {
    throw new Error("WebAudio not supported in this browser");
  }
  if (!state.audioCtx) {
    try {
      state.audioCtx = new AudioContextClass({ sampleRate: AUDIO_SAMPLE_RATE });
    } catch {
      state.audioCtx = new AudioContextClass();
    }
  }
  return state.audioCtx;
}

async function unlockAudio() {
  const ctx = ensureAudioContext();
  if (ctx.state !== "running") {
    await ctx.resume();
  }
  return ctx;
}

function clearImage() {
  if (state.imageUrl) {
    URL.revokeObjectURL(state.imageUrl);
    state.imageUrl = null;
  }
  elements.video.removeAttribute("src");
}

function resetAudioQueue() {
  state.nextAudioTime = 0;
}

function schedulePcm16(arrayBuffer) {
  const ctx = state.audioCtx;
  if (!ctx) {
    return;
  }

  const count = Math.floor(arrayBuffer.byteLength / 2);
  if (count <= 0) {
    return;
  }

  const view = new DataView(arrayBuffer);
  const audioBuffer = ctx.createBuffer(1, count, AUDIO_SAMPLE_RATE);
  const output = audioBuffer.getChannelData(0);
  for (let i = 0; i < count; i++) {
    output[i] = Math.max(-1, Math.min(1, view.getInt16(i * 2, true) / 32768));
  }

  const source = ctx.createBufferSource();
  source.buffer = audioBuffer;
  source.connect(ctx.destination);

  const now = ctx.currentTime;
  if (state.nextAudioTime < now + AUDIO_LEAD_SECONDS) {
    state.nextAudioTime = now + AUDIO_LEAD_SECONDS;
  }
  source.start(state.nextAudioTime);
  state.nextAudioTime += count / AUDIO_SAMPLE_RATE;
  if (state.nextAudioTime > now + AUDIO_MAX_BUFFER_SECONDS) {
    state.nextAudioTime = now + AUDIO_RESET_SECONDS;
  }
}

function parseKvLine(line) {
  const parts = String(line || "").trim().split(/\s+/);
  const parsed = { _type: parts.shift() || "" };
  for (const part of parts) {
    const eq = part.indexOf("=");
    if (eq <= 0) {
      continue;
    }
    parsed[part.slice(0, eq)] = part.slice(eq + 1);
  }
  return parsed;
}

function markBoardOnline() {
  const firstPresence = !state.boardPresenceSeen;
  state.boardPresenceSeen = true;
  elements.connectionPill.textContent = "device online";
  if (state.boardPresenceTimer) {
    clearTimeout(state.boardPresenceTimer);
    state.boardPresenceTimer = null;
  }
  if (firstPresence) {
    setNotice("Broker connected and ESP32 is online.", "info");
  }
}

function updateDeviceState(line) {
  const parsed = parseKvLine(line);
  if (parsed._type !== "state") {
    return;
  }

  markBoardOnline();
  elements.cameraStatus.textContent = parsed.camera || "-";
  elements.micStatus.textContent = parsed.mic || "-";
  elements.speakerStatus.textContent = parsed.speaker || "-";
  elements.heapStatus.textContent = parsed.heap || "-";
  elements.staStatus.textContent = parsed.sta || "-";
  elements.apStatus.textContent = parsed.ap || "-";
  elements.brokerStatus.textContent = parsed.broker || "-";
  elements.deviceState.textContent = `camera=${parsed.camera || "-"} mic=${parsed.mic || "-"} speaker=${parsed.speaker || "-"}`;
}

function publishControl() {
  if (!state.mqtt || !state.topics || !state.mqtt.connected) {
    return;
  }
  const sub = state.subscription;
  const line = `sub video=${sub.video ? 1 : 0} audio=${sub.audio ? 1 : 0} ch=${sub.channel} gain=${sub.gain} shift=${sub.shift}`;
  state.mqtt.publish(state.topics.control, line, { qos: 0, retain: true });
  log(`-> ${line}`);
}

function onMqttMessage(topic, payload) {
  if (!state.topics) {
    return;
  }

  if (topic === state.topics.video) {
    if (state.imageUrl) {
      URL.revokeObjectURL(state.imageUrl);
    }
    state.imageUrl = URL.createObjectURL(new Blob([payload], { type: "image/jpeg" }));
    elements.video.src = state.imageUrl;
    return;
  }

  if (topic === state.topics.audio && state.subscription.audio) {
    const data = payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.byteLength);
    schedulePcm16(data);
    return;
  }

  if (topic === state.topics.state) {
    const line = new TextDecoder().decode(payload);
    updateDeviceState(line);
    log(`< - ${line}`);
    return;
  }

  if (topic === state.topics.presence) {
    const presence = new TextDecoder().decode(payload).trim();
    if (presence === "1") {
      markBoardOnline();
    } else {
      state.boardPresenceSeen = false;
      elements.connectionPill.textContent = "device offline";
      setNotice("Broker connected, but ESP32 is offline or uses another shared key.", "warn");
    }
  }
}

async function connectMqtt() {
  const settings = requireSettings();
  await unlockAudio();
  disconnectMqtt();

  state.topics = buildTopics(settings);
  state.boardPresenceSeen = false;
  if (state.boardPresenceTimer) {
    clearTimeout(state.boardPresenceTimer);
    state.boardPresenceTimer = null;
  }

  state.mqtt = mqtt.connect(settings.brokerUrl, {
    clean: true,
    keepalive: 30,
    reconnectPeriod: 3000,
    connectTimeout: 15000,
    clientId: `viewer-${settings.deviceId}-${Math.random().toString(16).slice(2, 10)}`,
  });

  state.mqtt.on("connect", () => {
    elements.connectionPill.textContent = "broker connected";
    setNotice("Broker connected. Waiting for the ESP32 to appear on MQTT...", "info");
    log(`Connected to ${settings.brokerUrl}`);
    state.mqtt.subscribe(
      [state.topics.video, state.topics.audio, state.topics.state, state.topics.presence],
      { qos: 0 },
      (error) => {
        if (error) {
          setNotice(`Subscribe error: ${error.message}`, "error");
          log(`Subscribe error: ${error.message}`);
          return;
        }
        publishControl();
        state.boardPresenceTimer = setTimeout(() => {
          if (!state.boardPresenceSeen) {
            setNotice("Broker connected, but the board did not appear. Most often this means the ESP32 is offline, uses another shared key, or is not on the MQTT firmware.", "warn");
          }
        }, 6000);
      }
    );
  });

  state.mqtt.on("message", onMqttMessage);
  state.mqtt.on("reconnect", () => {
    elements.connectionPill.textContent = "reconnecting";
    setNotice("Reconnecting to MQTT broker...", "warn");
  });
  state.mqtt.on("close", () => {
    elements.connectionPill.textContent = "disconnected";
    setNotice("Disconnected from MQTT broker.", "warn");
  });
  state.mqtt.on("error", (error) => {
    setNotice(`MQTT error: ${error.message}`, "error");
    log(`MQTT error: ${error.message}`);
  });
}

function stopTalkbackInternals() {
  if (state.talkbackProcessor) {
    state.talkbackProcessor.disconnect();
    state.talkbackProcessor.onaudioprocess = null;
    state.talkbackProcessor = null;
  }
  if (state.talkbackSource) {
    state.talkbackSource.disconnect();
    state.talkbackSource = null;
  }
  if (state.talkbackSink) {
    state.talkbackSink.disconnect();
    state.talkbackSink = null;
  }
  if (state.talkbackStream) {
    state.talkbackStream.getTracks().forEach((track) => track.stop());
    state.talkbackStream = null;
  }
}

function disconnectMqtt() {
  stopTalkbackInternals();
  state.talkbackRestoreSubscription = null;
  if (state.mqtt) {
    try {
      state.mqtt.end(true);
    } catch {
    }
    state.mqtt = null;
  }
  state.topics = null;
  state.boardPresenceSeen = false;
  if (state.boardPresenceTimer) {
    clearTimeout(state.boardPresenceTimer);
    state.boardPresenceTimer = null;
  }
  clearImage();
  resetAudioQueue();
  elements.connectionPill.textContent = "offline";
  elements.talkbackState.textContent = "idle";
  setNotice("Disconnected. Enter the shared key from the ESP32 firmware and connect again.", "info");
}

function downsampleToInt16(input, inputRate, targetRate, gain = 1) {
  if (!input || input.length === 0) {
    return new Int16Array(0);
  }

  const ratio = inputRate / targetRate;
  const outLength = Math.max(1, Math.floor(input.length / ratio));
  const pcm = new Int16Array(outLength);
  let offset = 0;

  for (let i = 0; i < outLength; i++) {
    const next = Math.min(input.length, Math.floor((i + 1) * ratio));
    let sum = 0;
    let count = 0;
    for (let j = offset; j < next; j++) {
      sum += input[j];
      count++;
    }
    const averaged = (count ? sum / count : input[Math.min(offset, input.length - 1)]) * gain;
    const clipped = Math.max(-1, Math.min(1, averaged));
    pcm[i] = clipped < 0 ? clipped * 32768 : clipped * 32767;
    offset = Math.max(next, offset + 1);
  }

  return pcm;
}

function stopTalkback() {
  const restoreSubscription = state.talkbackRestoreSubscription;
  stopTalkbackInternals();
  state.talkbackRestoreSubscription = null;
  if (restoreSubscription) {
    state.subscription.video = restoreSubscription.video;
    state.subscription.audio = restoreSubscription.audio;
    state.subscription.channel = restoreSubscription.channel;
    state.subscription.gain = restoreSubscription.gain;
    state.subscription.shift = restoreSubscription.shift;
    resetAudioQueue();
    publishControl();
  }
  elements.talkbackState.textContent = "idle";
  setNotice("Talkback stopped.", "info");
}

async function startTalkback(gain) {
  if (!state.mqtt || !state.topics || !state.mqtt.connected) {
    throw new Error("Connect to MQTT first");
  }

  const ctx = await unlockAudio();
  const restoreSubscription = state.talkbackRestoreSubscription || { ...state.subscription };
  stopTalkbackInternals();
  state.talkbackRestoreSubscription = restoreSubscription;
  state.subscription.video = false;
  state.subscription.audio = false;
  publishControl();
  resetAudioQueue();

  state.talkbackStream = await navigator.mediaDevices.getUserMedia({
    audio: {
      channelCount: 1,
      echoCancellation: true,
      noiseSuppression: true,
      autoGainControl: true,
    },
  });

  state.talkbackGain = gain;
  state.talkbackSource = ctx.createMediaStreamSource(state.talkbackStream);
  state.talkbackProcessor = ctx.createScriptProcessor(2048, 1, 1);
  state.talkbackSink = ctx.createGain();
  state.talkbackSink.gain.value = 0;

  state.talkbackProcessor.onaudioprocess = (event) => {
    if (!state.mqtt || !state.mqtt.connected || !state.topics) {
      return;
    }
    const pcm = downsampleToInt16(
      event.inputBuffer.getChannelData(0),
      ctx.sampleRate,
      AUDIO_SAMPLE_RATE,
      state.talkbackGain
    );
    if (pcm.length > 0) {
      const bytes = new Uint8Array(pcm.buffer.slice(0));
      state.mqtt.publish(state.topics.speaker, bytes, { qos: 0, retain: false });
    }
  };

  state.talkbackSource.connect(state.talkbackProcessor);
  state.talkbackProcessor.connect(state.talkbackSink);
  state.talkbackSink.connect(ctx.destination);
  elements.talkbackState.textContent = `active x${gain}`;
  setNotice("Talkback started. Board audio is temporarily paused to keep the channel stable.", "info");
  log("Talkback started");
}

function bindEvents() {
  elements.saveSettings.addEventListener("click", saveSettings);
  elements.connect.addEventListener("click", () => connectMqtt().catch((error) => {
    setNotice(error.message, "error");
    log(error.message);
  }));
  elements.disconnect.addEventListener("click", disconnectMqtt);

  elements.startAv.addEventListener("click", () => {
    state.subscription.video = true;
    state.subscription.audio = true;
    resetAudioQueue();
    publishControl();
  });

  elements.videoOnly.addEventListener("click", () => {
    state.subscription.video = true;
    state.subscription.audio = false;
    resetAudioQueue();
    publishControl();
  });

  elements.audioStop.addEventListener("click", () => {
    state.subscription.audio = false;
    resetAudioQueue();
    publishControl();
  });

  elements.audioLeft.addEventListener("click", () => {
    state.subscription.audio = true;
    state.subscription.channel = "left";
    resetAudioQueue();
    publishControl();
  });

  elements.audioRight.addEventListener("click", () => {
    state.subscription.audio = true;
    state.subscription.channel = "right";
    resetAudioQueue();
    publishControl();
  });

  elements.audioLouder.addEventListener("click", () => {
    state.subscription.audio = true;
    state.subscription.gain = Math.min(8, state.subscription.gain + 1);
    publishControl();
  });

  elements.audioQuieter.addEventListener("click", () => {
    state.subscription.audio = true;
    state.subscription.gain = Math.max(1, state.subscription.gain - 1);
    publishControl();
  });

  elements.talkbackStart.addEventListener("click", () => {
    startTalkback(2).catch((error) => {
      elements.talkbackState.textContent = "failed";
      setNotice(error.message, "error");
      log(error.message);
    });
  });

  elements.talkbackLouder.addEventListener("click", () => {
    startTalkback(4).catch((error) => {
      elements.talkbackState.textContent = "failed";
      setNotice(error.message, "error");
      log(error.message);
    });
  });

  elements.talkbackStop.addEventListener("click", stopTalkback);
}

loadSettings();
bindEvents();

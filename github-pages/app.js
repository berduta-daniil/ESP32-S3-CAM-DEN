const SETTINGS_KEY = "esp32-s3-cam-den-hybrid-settings";
const AUDIO_SAMPLE_RATE = 16000;
const VIDEO_PACKET = 1;
const AUDIO_PACKET = 2;
const AUDIO_LEAD_SECONDS = 0.02;
const AUDIO_MAX_BUFFER_SECONDS = 0.25;
const AUDIO_RESET_SECONDS = 0.08;
const MEDIA_RECONNECT_MS = 3000;

const elements = {
  brokerUrl: document.getElementById("broker-url"),
  brokerUsername: document.getElementById("broker-username"),
  brokerPassword: document.getElementById("broker-password"),
  relayUrl: document.getElementById("relay-url"),
  viewerToken: document.getElementById("viewer-token"),
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
  mediaWs: null,
  speakerWs: null,
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
  talkbackRestoreAudio: null,
  topics: null,
  boardPresenceSeen: false,
  boardPresenceTimer: null,
  desiredConnection: false,
  mediaReconnectTimer: null,
  currentSettings: null,
};

function log(message) {
  const stamp = new Date().toLocaleTimeString();
  elements.log.textContent = `[${stamp}] ${message}\n${elements.log.textContent}`.trim();
}

function setNotice(text, kind = "info") {
  elements.notice.textContent = text;
  elements.notice.className = `notice notice-${kind}`;
}

function setConnectionPill() {
  const brokerConnected = Boolean(state.mqtt?.connected);
  const mediaConnected = Boolean(state.mediaWs && state.mediaWs.readyState === WebSocket.OPEN);
  if (state.boardPresenceSeen && brokerConnected && mediaConnected) {
    elements.connectionPill.textContent = "device online";
    return;
  }
  if (brokerConnected && mediaConnected) {
    elements.connectionPill.textContent = "broker+media";
    return;
  }
  if (brokerConnected) {
    elements.connectionPill.textContent = "broker only";
    return;
  }
  if (mediaConnected) {
    elements.connectionPill.textContent = "media only";
    return;
  }
  elements.connectionPill.textContent = "offline";
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
    brokerUsername: elements.brokerUsername.value.trim(),
    brokerPassword: elements.brokerPassword.value,
    relayUrl: elements.relayUrl.value.trim(),
    viewerToken: elements.viewerToken.value.trim(),
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
  elements.brokerUsername.value = settings.brokerUsername || "";
  elements.brokerPassword.value = settings.brokerPassword || "";
  elements.relayUrl.value = settings.relayUrl || "";
  elements.viewerToken.value = settings.viewerToken || "";
  elements.deviceId.value = settings.deviceId || "esp32-s3-cam-den-01";
  elements.sharedKey.value = settings.sharedKey || "";
}

function requireSettings() {
  const settings = saveSettings();
  if (!settings.brokerUrl || !settings.relayUrl || !settings.viewerToken || !settings.deviceId || !settings.sharedKey) {
    setNotice("Fill Broker WSS URL, Relay HTTPS URL, Viewer token, Device ID and Shared key before connecting.", "error");
    throw new Error("Missing broker/relay settings");
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
    state: `${prefix}/state`,
    presence: `${prefix}/presence`,
  };
}

function buildRelayWsUrl(kind, settings) {
  const relay = new URL(settings.relayUrl);
  const protocol = relay.protocol === "https:" ? "wss:" : "ws:";
  const params = new URLSearchParams({
    deviceId: settings.deviceId,
    viewerToken: settings.viewerToken,
  });
  return `${protocol}//${relay.host}/ws/viewer/${kind}?${params.toString()}`;
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
  setConnectionPill();
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

function handleMqttMessage(topic, payload) {
  if (!state.topics) {
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
      setConnectionPill();
      setNotice("Broker connected, but ESP32 is offline or uses another shared key.", "warn");
    }
  }
}

function closeSocket(socket) {
  if (!socket) {
    return;
  }
  try {
    socket.close();
  } catch {
  }
}

function clearMediaReconnect() {
  if (state.mediaReconnectTimer) {
    clearTimeout(state.mediaReconnectTimer);
    state.mediaReconnectTimer = null;
  }
}

function scheduleMediaReconnect() {
  if (!state.desiredConnection || state.mediaReconnectTimer || !state.currentSettings) {
    return;
  }
  state.mediaReconnectTimer = setTimeout(() => {
    state.mediaReconnectTimer = null;
    connectMediaSocket(state.currentSettings);
  }, MEDIA_RECONNECT_MS);
}

function handleRelayMediaMessage(event) {
  const frame = new Uint8Array(event.data);
  if (frame.byteLength <= 1) {
    return;
  }

  const type = frame[0];
  const payload = frame.slice(1);

  if (type === VIDEO_PACKET) {
    if (state.imageUrl) {
      URL.revokeObjectURL(state.imageUrl);
    }
    state.imageUrl = URL.createObjectURL(new Blob([payload], { type: "image/jpeg" }));
    elements.video.src = state.imageUrl;
    return;
  }

  if (type === AUDIO_PACKET && state.subscription.audio) {
    const bytes = payload.byteOffset === 0 && payload.byteLength === payload.buffer.byteLength
      ? payload.buffer
      : payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.byteLength);
    schedulePcm16(bytes);
  }
}

function connectMediaSocket(settings) {
  if (!state.desiredConnection) {
    return;
  }

  closeSocket(state.mediaWs);
  clearMediaReconnect();

  const mediaUrl = buildRelayWsUrl("media", settings);
  const mediaWs = new WebSocket(mediaUrl);
  mediaWs.binaryType = "arraybuffer";
  state.mediaWs = mediaWs;

  mediaWs.onopen = () => {
    if (state.mediaWs !== mediaWs) {
      mediaWs.close();
      return;
    }
    setConnectionPill();
    log(`Connected relay media ${mediaUrl}`);
    setNotice("Broker control/state connected. Relay media channel is live.", "info");
  };

  mediaWs.onmessage = handleRelayMediaMessage;

  mediaWs.onclose = () => {
    if (state.mediaWs === mediaWs) {
      state.mediaWs = null;
      setConnectionPill();
      log("Relay media socket closed");
      if (state.desiredConnection) {
        setNotice("Relay media channel disconnected. Reconnecting...", "warn");
        scheduleMediaReconnect();
      }
    }
  };

  mediaWs.onerror = () => {
    if (state.mediaWs === mediaWs) {
      log("Relay media socket error");
    }
  };
}

function connectMqtt(settings) {
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
    username: settings.brokerUsername || undefined,
    password: settings.brokerPassword || undefined,
  });

  state.mqtt.on("connect", () => {
    setConnectionPill();
    setNotice("Broker connected. Waiting for the ESP32 state and relay media...", "info");
    log(`Connected to ${settings.brokerUrl}`);
    state.mqtt.subscribe(
      [state.topics.state, state.topics.presence],
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
            setNotice("Broker connected, but the board did not appear. Most often this means the ESP32 is offline, uses another shared key, or is not on the expected firmware.", "warn");
          }
        }, 6000);
      }
    );
  });

  state.mqtt.on("message", handleMqttMessage);
  state.mqtt.on("reconnect", () => {
    setConnectionPill();
    setNotice("Reconnecting to MQTT broker...", "warn");
  });
  state.mqtt.on("close", () => {
    setConnectionPill();
    if (state.desiredConnection) {
      setNotice("Disconnected from MQTT broker. Waiting for reconnect...", "warn");
    }
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

function stopTalkback() {
  const restoreAudio = state.talkbackRestoreAudio;
  closeSocket(state.speakerWs);
  state.speakerWs = null;
  stopTalkbackInternals();
  if (restoreAudio !== null) {
    state.subscription.audio = restoreAudio;
    state.talkbackRestoreAudio = null;
    resetAudioQueue();
    publishControl();
  }
  elements.talkbackState.textContent = "idle";
  setNotice("Talkback stopped.", "info");
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

async function startTalkback(gain) {
  if (!state.desiredConnection || !state.currentSettings) {
    throw new Error("Connect first");
  }

  const ctx = await unlockAudio();
  const settings = state.currentSettings;
  stopTalkback();

  state.talkbackRestoreAudio = state.subscription.audio;
  state.subscription.audio = false;
  resetAudioQueue();
  publishControl();

  try {
    state.talkbackStream = await navigator.mediaDevices.getUserMedia({
      audio: {
        channelCount: 1,
        echoCancellation: true,
        noiseSuppression: true,
        autoGainControl: true,
      },
    });
  } catch (error) {
    state.subscription.audio = state.talkbackRestoreAudio;
    state.talkbackRestoreAudio = null;
    publishControl();
    throw error;
  }

  state.talkbackGain = gain;
  const speakerUrl = buildRelayWsUrl("speaker", settings);
  const speakerWs = new WebSocket(speakerUrl);
  speakerWs.binaryType = "arraybuffer";
  state.speakerWs = speakerWs;

  speakerWs.onopen = () => {
    if (state.speakerWs !== speakerWs) {
      speakerWs.close();
      return;
    }
    state.talkbackSource = ctx.createMediaStreamSource(state.talkbackStream);
    state.talkbackProcessor = ctx.createScriptProcessor(1024, 1, 1);
    state.talkbackSink = ctx.createGain();
    state.talkbackSink.gain.value = 0;
    state.talkbackProcessor.onaudioprocess = (event) => {
      if (!state.speakerWs || state.speakerWs.readyState !== WebSocket.OPEN) {
        return;
      }
      const pcm = downsampleToInt16(
        event.inputBuffer.getChannelData(0),
        ctx.sampleRate,
        AUDIO_SAMPLE_RATE,
        state.talkbackGain
      );
      if (pcm.length > 0) {
        state.speakerWs.send(pcm.buffer);
      }
    };
    state.talkbackSource.connect(state.talkbackProcessor);
    state.talkbackProcessor.connect(state.talkbackSink);
    state.talkbackSink.connect(ctx.destination);
    elements.talkbackState.textContent = `active x${gain}`;
    setNotice("Talkback started. Board playback is muted while you speak to avoid echo.", "info");
    log(`Talkback started via ${speakerUrl}`);
  };

  speakerWs.onclose = () => {
    if (state.speakerWs === speakerWs) {
      state.speakerWs = null;
      stopTalkbackInternals();
      if (state.talkbackRestoreAudio !== null) {
        state.subscription.audio = state.talkbackRestoreAudio;
        state.talkbackRestoreAudio = null;
        resetAudioQueue();
        publishControl();
      }
      elements.talkbackState.textContent = "idle";
      log("Talkback socket closed");
    }
  };

  speakerWs.onerror = () => {
    if (state.speakerWs === speakerWs) {
      setNotice("Talkback socket error.", "error");
      log("Talkback socket error");
    }
  };
}

function disconnect() {
  state.desiredConnection = false;
  state.currentSettings = null;
  clearMediaReconnect();
  closeSocket(state.mediaWs);
  state.mediaWs = null;
  stopTalkback();
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
  elements.deviceState.textContent = "waiting for control/state...";
  setConnectionPill();
  setNotice("Disconnected. Reconnect both MQTT and relay when ready.", "info");
}

async function connect() {
  const settings = requireSettings();
  await unlockAudio();
  disconnect();

  state.desiredConnection = true;
  state.currentSettings = settings;
  connectMqtt(settings);
  connectMediaSocket(settings);
}

function bindEvents() {
  elements.saveSettings.addEventListener("click", saveSettings);
  elements.connect.addEventListener("click", () => connect().catch((error) => {
    setNotice(error.message, "error");
    log(error.message);
  }));
  elements.disconnect.addEventListener("click", disconnect);

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
  window.addEventListener("beforeunload", disconnect);
}

loadSettings();
bindEvents();

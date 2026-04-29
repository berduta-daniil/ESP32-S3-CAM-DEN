const SETTINGS = {
  brokerUrl: "wss://rfaa3fd6.ala.eu-central-1.emqxsl.com:8084/mqtt",
  brokerUsername: "web-viewer",
  brokerPassword: "web-viewer",
  relayUrl: "https://esp32-s3-cam-den-relay.onrender.com",
  viewerToken: "esp32-viewer-token-2026-04-26-q5n8r3t1z6",
  deviceId: "esp32-s3-cam-den-01",
  sharedKey: "esp32-cam-den-private-2026-04-26-x7m9k2p4q8",
};

const AUDIO_SAMPLE_RATE = 16000;
const VIDEO_PACKET = 1;
const AUDIO_PACKET = 2;
const AUDIO_LEAD_SECONDS = 0.02;
const AUDIO_MAX_BUFFER_SECONDS = 0.25;
const AUDIO_RESET_SECONDS = 0.08;
const MEDIA_RECONNECT_MS = 3000;

const elements = {
  notice: document.getElementById("notice"),
  connectionPill: document.getElementById("connection-pill"),
  deviceState: document.getElementById("device-state"),
  video: document.getElementById("video"),
  talkbackToggle: document.getElementById("talkback-toggle"),
  talkbackState: document.getElementById("talkback-state"),
};

const state = {
  mqtt: null,
  mediaWs: null,
  speakerWs: null,
  audioCtx: null,
  nextAudioTime: 0,
  imageUrl: null,
  desiredConnection: false,
  currentSettings: SETTINGS,
  mediaReconnectTimer: null,
  boardPresenceSeen: false,
  boardPresenceTimer: null,
  topics: null,
  subscription: {
    video: true,
    audio: true,
    channel: "right",
    gain: 4,
    shift: 0,
  },
  talkbackStream: null,
  talkbackSource: null,
  talkbackProcessor: null,
  talkbackSink: null,
  talkbackGain: 2,
  talkbackRestoreAudio: null,
};

function setNotice(text, kind = "info") {
  elements.notice.textContent = text;
  elements.notice.className = `notice notice-${kind}`;
}

function setConnectionPill() {
  const brokerConnected = Boolean(state.mqtt?.connected);
  const mediaConnected = Boolean(state.mediaWs && state.mediaWs.readyState === WebSocket.OPEN);
  if (state.boardPresenceSeen && brokerConnected && mediaConnected) {
    elements.connectionPill.textContent = "online";
    return;
  }
  if (brokerConnected && mediaConnected) {
    elements.connectionPill.textContent = "connecting";
    return;
  }
  if (brokerConnected || mediaConnected) {
    elements.connectionPill.textContent = "loading";
    return;
  }
  elements.connectionPill.textContent = "offline";
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
    throw new Error("WebAudio is not supported in this browser");
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
  if (!ctx || ctx.state !== "running") {
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
  state.boardPresenceSeen = true;
  if (state.boardPresenceTimer) {
    clearTimeout(state.boardPresenceTimer);
    state.boardPresenceTimer = null;
  }
  setConnectionPill();
}

function publishControl() {
  if (!state.mqtt || !state.topics || !state.mqtt.connected) {
    return;
  }
  const sub = state.subscription;
  const line = `sub video=${sub.video ? 1 : 0} audio=${sub.audio ? 1 : 0} ch=${sub.channel} gain=${sub.gain} shift=${sub.shift}`;
  state.mqtt.publish(state.topics.control, line, { qos: 0, retain: true });
}

function handleMqttMessage(topic, payload) {
  if (!state.topics) {
    return;
  }

  if (topic === state.topics.state) {
    const line = new TextDecoder().decode(payload);
    const parsed = parseKvLine(line);
    if (parsed._type === "state") {
      markBoardOnline();
      elements.deviceState.textContent = `camera=${parsed.camera || "-"} mic=${parsed.mic || "-"} speaker=${parsed.speaker || "-"}`;
      setNotice("Board video and audio are connected.", "info");
    }
    return;
  }

  if (topic === state.topics.presence) {
    const parsed = parseKvLine(new TextDecoder().decode(payload));
    if (parsed.device === "1") {
      markBoardOnline();
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
  if (!state.desiredConnection || state.mediaReconnectTimer) {
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
  };

  mediaWs.onmessage = handleRelayMediaMessage;

  mediaWs.onclose = () => {
    if (state.mediaWs === mediaWs) {
      state.mediaWs = null;
      setConnectionPill();
      if (state.desiredConnection) {
        setNotice("Media channel is reconnecting...", "warn");
        scheduleMediaReconnect();
      }
    }
  };

  mediaWs.onerror = () => {
    if (state.mediaWs === mediaWs) {
      setNotice("Media channel error. Reconnecting...", "warn");
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
    setNotice("Control channel connected. Waiting for board...", "info");
    state.mqtt.subscribe([state.topics.state, state.topics.presence], { qos: 0 }, (error) => {
      if (error) {
        setNotice(`MQTT subscribe error: ${error.message}`, "error");
        return;
      }
      publishControl();
      state.boardPresenceTimer = setTimeout(() => {
        if (!state.boardPresenceSeen) {
          setNotice("Board has not replied yet. This can take a few seconds after reconnect.", "warn");
        }
      }, 6000);
    });
  });

  state.mqtt.on("message", handleMqttMessage);
  state.mqtt.on("reconnect", () => {
    setConnectionPill();
    setNotice("MQTT is reconnecting...", "warn");
  });
  state.mqtt.on("close", () => {
    setConnectionPill();
    if (state.desiredConnection) {
      setNotice("MQTT disconnected. Waiting for reconnect...", "warn");
    }
  });
  state.mqtt.on("error", (error) => {
    setNotice(`MQTT error: ${error.message}`, "error");
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

function updateTalkbackUi(active) {
  elements.talkbackToggle.textContent = active ? "Disable microphone" : "Enable microphone";
  elements.talkbackState.textContent = active ? "microphone on" : "off";
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
  updateTalkbackUi(false);
  setNotice("Microphone is off.", "info");
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

async function startTalkback() {
  if (!state.desiredConnection || !state.currentSettings) {
    throw new Error("The page is not connected to the board yet");
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
    updateTalkbackUi(true);
    setNotice("Microphone is on. Board audio is muted temporarily to avoid echo.", "info");
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
      updateTalkbackUi(false);
    }
  };

  speakerWs.onerror = () => {
    if (state.speakerWs === speakerWs) {
      setNotice("Talkback channel error.", "error");
    }
  };
}

function disconnect() {
  state.desiredConnection = false;
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
  setConnectionPill();
}

async function connect() {
  try {
    await unlockAudio();
  } catch {
  }

  disconnect();
  state.desiredConnection = true;
  connectMqtt(state.currentSettings);
  connectMediaSocket(state.currentSettings);
}

function bindEvents() {
  elements.talkbackToggle.addEventListener("click", () => {
    if (state.speakerWs) {
      stopTalkback();
      return;
    }
    startTalkback().catch((error) => {
      updateTalkbackUi(false);
      setNotice(error.message, "error");
    });
  });

  const unlockHandler = () => {
    unlockAudio()
      .then(() => {
        if (state.boardPresenceSeen) {
          setNotice("Audio is enabled.", "info");
        }
      })
      .catch(() => {
      });
  };

  window.addEventListener("pointerdown", unlockHandler, { passive: true });
  window.addEventListener("keydown", unlockHandler, { passive: true });
  window.addEventListener("beforeunload", disconnect);
}

bindEvents();
updateTalkbackUi(false);
connect();

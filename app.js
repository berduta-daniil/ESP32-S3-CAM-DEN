const SETTINGS_KEY = "esp32-cloud-viewer-settings";
const AUDIO_SAMPLE_RATE = 16000;
const VIDEO_PACKET = 1;
const AUDIO_PACKET = 2;

const elements = {
  relayUrl: document.getElementById("relay-url"),
  deviceId: document.getElementById("device-id"),
  viewerToken: document.getElementById("viewer-token"),
  saveSettings: document.getElementById("save-settings"),
  connect: document.getElementById("connect-btn"),
  disconnect: document.getElementById("disconnect-btn"),
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
};

const state = {
  controlWs: null,
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
};

function log(message) {
  const stamp = new Date().toLocaleTimeString();
  elements.log.textContent = `[${stamp}] ${message}\n${elements.log.textContent}`.trim();
}

function setConnectionState(text) {
  elements.connectionPill.textContent = text;
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
    relayUrl: elements.relayUrl.value.trim(),
    deviceId: elements.deviceId.value.trim(),
    viewerToken: elements.viewerToken.value.trim(),
  };
  localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
  log("Settings saved");
  return settings;
}

function loadSettings() {
  const settings = getSettings();
  elements.relayUrl.value = settings.relayUrl || "";
  elements.deviceId.value = settings.deviceId || "";
  elements.viewerToken.value = settings.viewerToken || "";
}

function requireSettings() {
  const settings = saveSettings();
  if (!settings.relayUrl || !settings.deviceId || !settings.viewerToken) {
    throw new Error("Fill relay URL, device ID and viewer token");
  }
  return settings;
}

function relayBase(settings) {
  const url = new URL(settings.relayUrl);
  return `${url.protocol}//${url.host}`;
}

function relayWsBase(settings) {
  const url = new URL(settings.relayUrl);
  const protocol = url.protocol === "https:" ? "wss:" : "ws:";
  return `${protocol}//${url.host}`;
}

function buildWsUrl(kind, settings) {
  const base = relayWsBase(settings);
  const params = new URLSearchParams({
    deviceId: settings.deviceId,
    viewerToken: settings.viewerToken,
  });
  return `${base}/ws/browser/${kind}?${params.toString()}`;
}

function ensureAudioContext() {
  const AudioContextClass = window.AudioContext || window.webkitAudioContext;
  if (!AudioContextClass) {
    throw new Error("WebAudio not supported");
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

function closeSocket(socket) {
  if (!socket) {
    return;
  }
  try {
    socket.close();
  } catch {
  }
}

function resetImage() {
  if (state.imageUrl) {
    URL.revokeObjectURL(state.imageUrl);
    state.imageUrl = null;
  }
  elements.video.removeAttribute("src");
}

function schedulePcm16(buffer) {
  const ctx = state.audioCtx;
  if (!ctx) {
    return;
  }

  const view = new DataView(buffer);
  const count = Math.floor(buffer.byteLength / 2);
  if (count <= 0) {
    return;
  }

  const audioBuffer = ctx.createBuffer(1, count, AUDIO_SAMPLE_RATE);
  const output = audioBuffer.getChannelData(0);
  for (let i = 0; i < count; i++) {
    output[i] = Math.max(-1, Math.min(1, view.getInt16(i * 2, true) / 32768));
  }

  const source = ctx.createBufferSource();
  source.buffer = audioBuffer;
  source.connect(ctx.destination);

  const now = ctx.currentTime;
  if (state.nextAudioTime < now + 0.02) {
    state.nextAudioTime = now + 0.02;
  }
  source.start(state.nextAudioTime);
  state.nextAudioTime += count / AUDIO_SAMPLE_RATE;
  if (state.nextAudioTime > now + 0.25) {
    state.nextAudioTime = now + 0.08;
  }
}

function parseKvLine(line) {
  const result = {};
  const parts = line.trim().split(/\s+/);
  result._type = parts.shift() || "";
  for (const part of parts) {
    const eq = part.indexOf("=");
    if (eq <= 0) {
      continue;
    }
    const key = part.slice(0, eq);
    const value = part.slice(eq + 1);
    result[key] = value;
  }
  return result;
}

function updateStateFromLine(line) {
  const parsed = parseKvLine(line);
  if (parsed._type === "state") {
    elements.cameraStatus.textContent = parsed.camera || "-";
    elements.micStatus.textContent = parsed.mic || "-";
    elements.speakerStatus.textContent = parsed.speaker || "-";
    elements.heapStatus.textContent = parsed.heap || "-";
    elements.staStatus.textContent = parsed.sta || "-";
    elements.apStatus.textContent = parsed.ap || "-";
    elements.deviceState.textContent = `camera=${parsed.camera || "-"} mic=${parsed.mic || "-"} speaker=${parsed.speaker || "-"}`;
  }
  if (parsed._type === "presence") {
    const device = parsed.device === "1" ? "online" : "offline";
    elements.connectionPill.textContent = `relay: ${device}`;
  }
}

function sendSubscription() {
  if (!state.controlWs || state.controlWs.readyState !== WebSocket.OPEN) {
    return;
  }
  const sub = state.subscription;
  const line = `sub video=${sub.video ? 1 : 0} audio=${sub.audio ? 1 : 0} ch=${sub.channel} gain=${sub.gain} shift=${sub.shift}`;
  state.controlWs.send(line);
  log(`-> ${line}`);
}

function handleMediaMessage(event) {
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
    schedulePcm16(payload.buffer);
  }
}

async function connect() {
  const settings = requireSettings();
  await unlockAudio();
  disconnect();

  const controlUrl = buildWsUrl("control", settings);
  const mediaUrl = buildWsUrl("media", settings);

  state.controlWs = new WebSocket(controlUrl);
  state.mediaWs = new WebSocket(mediaUrl);
  state.mediaWs.binaryType = "arraybuffer";

  state.controlWs.onopen = () => {
    setConnectionState("control connected");
    log(`Connected control ${controlUrl}`);
    sendSubscription();
  };
  state.controlWs.onmessage = (event) => {
    const line = String(event.data || "");
    updateStateFromLine(line);
    log(`< - ${line}`);
  };
  state.controlWs.onclose = () => {
    setConnectionState("control disconnected");
    state.controlWs = null;
  };
  state.controlWs.onerror = () => {
    log("Control socket error");
  };

  state.mediaWs.onopen = () => {
    setConnectionState("media connected");
    log(`Connected media ${mediaUrl}`);
  };
  state.mediaWs.onmessage = handleMediaMessage;
  state.mediaWs.onclose = () => {
    state.mediaWs = null;
    log("Media socket closed");
  };
  state.mediaWs.onerror = () => {
    log("Media socket error");
  };
}

function disconnect() {
  closeSocket(state.controlWs);
  closeSocket(state.mediaWs);
  state.controlWs = null;
  state.mediaWs = null;
  stopTalkback();
  resetImage();
  setConnectionState("offline");
}

function downsampleToInt16(input, inputRate, targetRate, gain = 1) {
  if (!input || input.length === 0) {
    return new Int16Array(0);
  }

  if (targetRate === inputRate) {
    const pcm = new Int16Array(input.length);
    for (let i = 0; i < input.length; i++) {
      const sample = Math.max(-1, Math.min(1, input[i])) * gain;
      const clipped = Math.max(-1, Math.min(1, sample));
      pcm[i] = clipped < 0 ? clipped * 32768 : clipped * 32767;
    }
    return pcm;
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
  stopTalkbackInternals();
  closeSocket(state.speakerWs);
  state.speakerWs = null;
  elements.talkbackState.textContent = "idle";
}

async function startTalkback(gain) {
  const settings = requireSettings();
  await unlockAudio();
  stopTalkback();

  try {
    state.talkbackStream = await navigator.mediaDevices.getUserMedia({
      audio: {
        channelCount: 1,
        echoCancellation: false,
        noiseSuppression: false,
        autoGainControl: false,
      },
    });
  } catch (error) {
    log(`Talkback microphone failed: ${error.message}`);
    throw error;
  }

  state.talkbackGain = gain;
  state.speakerWs = new WebSocket(buildWsUrl("speaker", settings));
  state.speakerWs.binaryType = "arraybuffer";

  state.speakerWs.onopen = () => {
    const ctx = ensureAudioContext();
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
    log("Talkback started");
  };
  state.speakerWs.onclose = () => {
    stopTalkbackInternals();
    state.speakerWs = null;
    elements.talkbackState.textContent = "idle";
    log("Talkback stopped");
  };
  state.speakerWs.onerror = () => {
    log("Talkback socket error");
  };
}

function bindEvents() {
  elements.saveSettings.addEventListener("click", saveSettings);
  elements.connect.addEventListener("click", () => connect().catch((error) => log(error.message)));
  elements.disconnect.addEventListener("click", disconnect);

  elements.startAv.addEventListener("click", () => {
    state.subscription.video = true;
    state.subscription.audio = true;
    sendSubscription();
  });

  elements.videoOnly.addEventListener("click", () => {
    state.subscription.video = true;
    state.subscription.audio = false;
    sendSubscription();
  });

  elements.audioStop.addEventListener("click", () => {
    state.subscription.audio = false;
    sendSubscription();
  });

  elements.audioLeft.addEventListener("click", () => {
    state.subscription.audio = true;
    state.subscription.channel = "left";
    sendSubscription();
  });

  elements.audioRight.addEventListener("click", () => {
    state.subscription.audio = true;
    state.subscription.channel = "right";
    sendSubscription();
  });

  elements.audioLouder.addEventListener("click", () => {
    state.subscription.audio = true;
    state.subscription.gain = Math.min(12, state.subscription.gain + 2);
    sendSubscription();
  });

  elements.audioQuieter.addEventListener("click", () => {
    state.subscription.audio = true;
    state.subscription.gain = Math.max(1, state.subscription.gain - 1);
    sendSubscription();
  });

  elements.talkbackStart.addEventListener("click", () => {
    startTalkback(2).catch((error) => {
      elements.talkbackState.textContent = "failed";
      log(error.message);
    });
  });

  elements.talkbackLouder.addEventListener("click", () => {
    startTalkback(4).catch((error) => {
      elements.talkbackState.textContent = "failed";
      log(error.message);
    });
  });

  elements.talkbackStop.addEventListener("click", stopTalkback);
}

loadSettings();
bindEvents();
elements.audio.src = "";

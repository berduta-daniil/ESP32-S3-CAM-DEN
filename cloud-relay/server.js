const http = require("http");
const WebSocket = require("ws");

const PORT = Number(process.env.PORT || 8080);

function loadDeviceConfigs() {
  if (process.env.DEVICES_JSON) {
    const parsed = JSON.parse(process.env.DEVICES_JSON);
    if (!Array.isArray(parsed) || parsed.length === 0) {
      throw new Error("DEVICES_JSON must be a non-empty array");
    }
    return parsed;
  }

  if (process.env.DEVICE_ID && process.env.DEVICE_TOKEN && process.env.VIEWER_TOKEN) {
    return [{
      id: process.env.DEVICE_ID,
      deviceToken: process.env.DEVICE_TOKEN,
      viewerToken: process.env.VIEWER_TOKEN,
    }];
  }

  throw new Error("Set DEVICES_JSON or DEVICE_ID/DEVICE_TOKEN/VIEWER_TOKEN");
}

const configs = new Map(loadDeviceConfigs().map((item) => [item.id, item]));
const devices = new Map();

function getDeviceState(deviceId) {
  if (!devices.has(deviceId)) {
    devices.set(deviceId, {
      id: deviceId,
      deviceControl: null,
      deviceMedia: null,
      deviceSpeaker: null,
      browserControls: new Set(),
      browserMedia: new Set(),
      browserSpeakers: new Set(),
      lastStateLine: "state camera=offline mic=offline speaker=offline",
      lastSubscriptionLine: "sub video=1 audio=1 ch=left gain=4 shift=0",
    });
  }
  return devices.get(deviceId);
}

function parseLine(line) {
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

function computeAggregateSubscription(state) {
  const controls = [...state.browserControls].filter((socket) => socket.readyState === WebSocket.OPEN);
  const last = controls.at(-1);
  const defaults = {
    video: false,
    audio: false,
    ch: "left",
    gain: "4",
    shift: "0",
  };

  if (!last || !last.subscription) {
    return defaults;
  }

  let video = false;
  let audio = false;
  for (const socket of controls) {
    if (!socket.subscription) {
      continue;
    }
    video = video || socket.subscription.video === "1";
    audio = audio || socket.subscription.audio === "1";
  }

  return {
    video,
    audio,
    ch: last.subscription.ch || "left",
    gain: last.subscription.gain || "4",
    shift: last.subscription.shift || "0",
  };
}

function broadcast(set, data, isBinary = false) {
  for (const socket of set) {
    if (socket.readyState === WebSocket.OPEN) {
      socket.send(data, { binary: isBinary });
    }
  }
}

function deviceOnline(state) {
  return Boolean(
    (state.deviceControl && state.deviceControl.readyState === WebSocket.OPEN) ||
    (state.deviceMedia && state.deviceMedia.readyState === WebSocket.OPEN) ||
    (state.deviceSpeaker && state.deviceSpeaker.readyState === WebSocket.OPEN)
  );
}

function presenceLine(state) {
  return `presence device=${deviceOnline(state) ? 1 : 0} viewers=${state.browserControls.size} talkback=${state.browserSpeakers.size}`;
}

function refreshPresence(state) {
  broadcast(state.browserControls, presenceLine(state));
}

function forwardSubscription(state) {
  const aggregate = computeAggregateSubscription(state);
  const line = `sub video=${aggregate.video ? 1 : 0} audio=${aggregate.audio ? 1 : 0} ch=${aggregate.ch} gain=${aggregate.gain} shift=${aggregate.shift}`;
  state.lastSubscriptionLine = line;
  if (state.deviceControl && state.deviceControl.readyState === WebSocket.OPEN) {
    state.deviceControl.send(line);
  }
  refreshPresence(state);
}

function authenticateDevice(url) {
  const deviceId = url.searchParams.get("deviceId") || "";
  const token = url.searchParams.get("deviceToken") || "";
  const config = configs.get(deviceId);
  if (!config || token !== config.deviceToken) {
    return null;
  }
  return { deviceId, config };
}

function authenticateViewer(url) {
  const deviceId = url.searchParams.get("deviceId") || "";
  const token = url.searchParams.get("viewerToken") || "";
  const config = configs.get(deviceId);
  if (!config || token !== config.viewerToken) {
    return null;
  }
  return { deviceId, config };
}

const server = http.createServer((req, res) => {
  if (req.url === "/health") {
    const summary = [...devices.values()].map((state) => ({
      id: state.id,
      deviceControl: Boolean(state.deviceControl && state.deviceControl.readyState === WebSocket.OPEN),
      deviceMedia: Boolean(state.deviceMedia && state.deviceMedia.readyState === WebSocket.OPEN),
      deviceSpeaker: Boolean(state.deviceSpeaker && state.deviceSpeaker.readyState === WebSocket.OPEN),
      deviceOnline: deviceOnline(state),
      viewers: state.browserControls.size,
      talkbackClients: state.browserSpeakers.size,
      lastStateLine: state.lastStateLine,
      lastSubscriptionLine: state.lastSubscriptionLine,
    }));
    res.writeHead(200, {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "no-store",
      "Access-Control-Allow-Origin": "*",
    });
    res.end(JSON.stringify({ ok: true, devices: summary }, null, 2));
    return;
  }

  res.writeHead(200, {
    "Content-Type": "text/plain; charset=utf-8",
    "Cache-Control": "no-store",
  });
  res.end("ESP32 cloud relay is running\n");
});

const wss = new WebSocket.Server({ noServer: true, perMessageDeflate: false });

server.on("upgrade", (req, socket, head) => {
  let url;
  try {
    url = new URL(req.url, `http://${req.headers.host}`);
  } catch {
    console.log(`[upgrade] invalid url: ${req.url || "<empty>"}`);
    socket.destroy();
    return;
  }

  const path = url.pathname;
  const isDevice = path.startsWith("/ws/device/");
  const auth = isDevice ? authenticateDevice(url) : authenticateViewer(url);
  if (!auth) {
    console.log(`[upgrade] unauthorized ${isDevice ? "device" : "viewer"} path=${path} query=${url.search}`);
    socket.write("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n");
    socket.destroy();
    return;
  }

  const kind = path.split("/").pop();
  if (!["control", "media", "speaker"].includes(kind)) {
    console.log(`[upgrade] unknown kind path=${path}`);
    socket.write("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
    socket.destroy();
    return;
  }

  wss.handleUpgrade(req, socket, head, (ws) => {
    ws.meta = {
      role: isDevice ? "device" : "browser",
      deviceId: auth.deviceId,
      kind,
    };
    wss.emit("connection", ws, req);
  });
});

wss.on("connection", (ws) => {
  const { role, deviceId, kind } = ws.meta;
  const state = getDeviceState(deviceId);
  console.log(`[ws] ${role}/${kind} connected for ${deviceId}`);

  if (role === "device") {
    if (kind === "control") {
      if (state.deviceControl) {
        state.deviceControl.close();
      }
      state.deviceControl = ws;
      ws.send(state.lastSubscriptionLine);
    } else if (kind === "media") {
      if (state.deviceMedia) {
        state.deviceMedia.close();
      }
      state.deviceMedia = ws;
    } else if (kind === "speaker") {
      if (state.deviceSpeaker) {
        state.deviceSpeaker.close();
      }
      state.deviceSpeaker = ws;
    }
    refreshPresence(state);
  } else {
    if (kind === "control") {
      ws.subscription = parseLine(state.lastSubscriptionLine);
      state.browserControls.add(ws);
      ws.send(presenceLine(state));
      ws.send(state.lastStateLine);
    } else if (kind === "media") {
      state.browserMedia.add(ws);
    } else if (kind === "speaker") {
      state.browserSpeakers.add(ws);
    }
    refreshPresence(state);
  }

  ws.on("message", (data, isBinary) => {
    if (role === "device") {
      if (kind === "control" && !isBinary) {
        const line = String(data).trim();
        if (line.startsWith("state ") || line.startsWith("hello ")) {
          state.lastStateLine = line;
          broadcast(state.browserControls, line);
        } else if (line === "pong") {
          broadcast(state.browserControls, "relay device_pong=1");
        }
        return;
      }
      if (kind === "media" && isBinary) {
        broadcast(state.browserMedia, data, true);
        return;
      }
      return;
    }

    if (kind === "control" && !isBinary) {
      const line = String(data).trim();
      const parsed = parseLine(line);
      if (parsed._type === "sub") {
        ws.subscription = parsed;
        forwardSubscription(state);
      }
      return;
    }

    if (kind === "speaker" && isBinary && state.deviceSpeaker && state.deviceSpeaker.readyState === WebSocket.OPEN) {
      state.deviceSpeaker.send(data, { binary: true });
    }
  });

  ws.on("close", () => {
    console.log(`[ws] ${role}/${kind} disconnected for ${deviceId}`);
    if (role === "device") {
      if (kind === "control" && state.deviceControl === ws) {
        state.deviceControl = null;
      }
      if (kind === "media" && state.deviceMedia === ws) {
        state.deviceMedia = null;
      }
      if (kind === "speaker" && state.deviceSpeaker === ws) {
        state.deviceSpeaker = null;
      }
      refreshPresence(state);
      return;
    }

    if (kind === "control") {
      state.browserControls.delete(ws);
      forwardSubscription(state);
      return;
    }
    if (kind === "media") {
      state.browserMedia.delete(ws);
    }
    if (kind === "speaker") {
      state.browserSpeakers.delete(ws);
    }
    refreshPresence(state);
  });
});

server.listen(PORT, () => {
  console.log(`ESP32 cloud relay listening on :${PORT}`);
  console.log(`Devices: ${[...configs.keys()].join(", ")}`);
});

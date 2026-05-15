"use strict";

const http = require("http");
const net = require("net");
const { exec } = require("child_process");

const ESP32_HOST = process.env.ESP32_HOST || "192.168.4.1";
const ESP32_HTTP_PORT = Number(process.env.ESP32_HTTP_PORT || 80);
const ESP32_AUDIO_PORT = Number(process.env.ESP32_AUDIO_PORT || 81);
const PROXY_HOST = process.env.PROXY_HOST || "0.0.0.0";
const PROXY_PORT = Number(process.env.PROXY_PORT || 8080);
const ESP32_AUTH_USER = process.env.ESP32_AUTH_USER || "cam";
const ESP32_AUTH_PASSWORD = process.env.ESP32_AUTH_PASSWORD || "1234";
const PROXY_TOKEN = process.env.PROXY_TOKEN || "";
const REQUEST_TIMEOUT_MS = Number(process.env.ESP32_REQUEST_TIMEOUT_MS || 15000);

function basicAuthHeader() {
  if (!ESP32_AUTH_USER && !ESP32_AUTH_PASSWORD) {
    return "";
  }
  return `Basic ${Buffer.from(`${ESP32_AUTH_USER}:${ESP32_AUTH_PASSWORD}`).toString("base64")}`;
}

function parseCookies(header) {
  const cookies = {};
  if (!header) {
    return cookies;
  }
  for (const part of header.split(";")) {
    const index = part.indexOf("=");
    if (index > -1) {
      cookies[part.slice(0, index).trim()] = decodeURIComponent(part.slice(index + 1).trim());
    }
  }
  return cookies;
}

function proxyAuthorized(req, parsedUrl, res) {
  if (!PROXY_TOKEN) {
    return true;
  }

  const token = parsedUrl.searchParams.get("token") || req.headers["x-proxy-token"] || "";
  const cookies = parseCookies(req.headers.cookie || "");
  if (token === PROXY_TOKEN || cookies.pc_proxy_token === PROXY_TOKEN) {
    if (token === PROXY_TOKEN && res) {
      res.setHeader("Set-Cookie", `pc_proxy_token=${encodeURIComponent(PROXY_TOKEN)}; Path=/; HttpOnly; SameSite=Lax`);
    }
    return true;
  }

  return false;
}

function sendText(res, status, text, contentType = "text/plain; charset=utf-8") {
  res.writeHead(status, {
    "Content-Type": contentType,
    "Cache-Control": "no-store",
  });
  res.end(text);
}

function sendUnauthorized(res) {
  sendText(
    res,
    401,
    `<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Proxy locked</title><style>body{font-family:system-ui,sans-serif;margin:32px;max-width:720px}input,button{font:inherit;padding:10px;margin-top:8px}input{min-width:280px}</style></head><body><h1>Proxy locked</h1><p>Open this page with <code>?token=YOUR_TOKEN</code>, or enter the token shown in the proxy window.</p><form method='GET' action='/'><label>Token<br><input name='token' autocomplete='off' autofocus></label><br><button type='submit'>Open video</button></form></body></html>`,
    "text/html; charset=utf-8"
  );
}

function checkEsp32Health() {
  return new Promise((resolve) => {
    const startedAt = Date.now();
    const req = http.request(
      {
        host: ESP32_HOST,
        port: ESP32_HTTP_PORT,
        method: "GET",
        path: "/health",
        timeout: 2500,
        headers: {
          Authorization: basicAuthHeader(),
          Connection: "close",
        },
      },
      (upstreamRes) => {
        let body = "";
        upstreamRes.setEncoding("utf8");
        upstreamRes.on("data", (chunk) => {
          if (body.length < 4096) body += chunk;
        });
        upstreamRes.on("end", () => {
          resolve({
            ok: upstreamRes.statusCode === 200,
            statusCode: upstreamRes.statusCode,
            elapsedMs: Date.now() - startedAt,
            target: `${ESP32_HOST}:${ESP32_HTTP_PORT}`,
            body,
          });
        });
      }
    );

    req.on("timeout", () => req.destroy(new Error("ESP32 health timed out")));
    req.on("error", (error) => {
      resolve({
        ok: false,
        error: error.message,
        elapsedMs: Date.now() - startedAt,
        target: `${ESP32_HOST}:${ESP32_HTTP_PORT}`,
      });
    });
    req.end();
  });
}

function proxyHttp(req, res, targetPort, targetPath, useBoardAuth) {
  const headers = {
    Host: `${ESP32_HOST}:${targetPort}`,
    Connection: "close",
    "User-Agent": "ESP32-S3-CAM-DEN-PC-Proxy/1.0",
  };

  if (req.headers.range) {
    headers.Range = req.headers.range;
  }

  const auth = useBoardAuth ? basicAuthHeader() : "";
  if (auth) {
    headers.Authorization = auth;
  }

  const upstream = http.request(
    {
      host: ESP32_HOST,
      port: targetPort,
      method: "GET",
      path: targetPath,
      headers,
      timeout: REQUEST_TIMEOUT_MS,
    },
    (upstreamRes) => {
      const responseHeaders = { ...upstreamRes.headers };
      delete responseHeaders["transfer-encoding"];
      responseHeaders["Cache-Control"] = "no-store";
      responseHeaders["Access-Control-Allow-Origin"] = "*";
      res.writeHead(upstreamRes.statusCode || 502, responseHeaders);
      upstreamRes.pipe(res);
    }
  );

  upstream.on("timeout", () => upstream.destroy(new Error("ESP32 request timed out")));
  upstream.on("error", (error) => {
    if (!res.headersSent) {
      sendText(res, 502, `ESP32 proxy error: ${error.message}\n`);
    } else {
      res.destroy(error);
    }
  });
  req.on("close", () => upstream.destroy());
  upstream.end();
}

function pageHtml() {
  const tokenQuery = PROXY_TOKEN ? "?token=..." : "";
  return `<!doctype html>
<html lang="uk">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-S3-CAM-DEN Proxy</title>
  <style>
    html,body{margin:0;min-height:100%;background:#050505;color:#f5f5f5;font-family:system-ui,-apple-system,Segoe UI,sans-serif}
    .stage{min-height:100vh;display:grid;place-items:center;background:#000}
    img{display:block;max-width:100vw;max-height:100vh;width:100vw;height:100vh;object-fit:contain;background:#000}
    .panel{position:fixed;left:12px;right:12px;bottom:12px;display:flex;gap:8px;align-items:center;flex-wrap:wrap;padding:10px;background:rgba(8,10,12,.72);backdrop-filter:blur(10px);border:1px solid rgba(255,255,255,.14);border-radius:8px}
    button,a{border:0;border-radius:6px;background:#2563eb;color:white;padding:9px 12px;font-weight:700;text-decoration:none;cursor:pointer}
    button.secondary,a.secondary{background:#374151}
    .status{color:#d1d5db;font-size:14px}
    .error{position:fixed;left:12px;top:12px;max-width:min(720px,calc(100vw - 24px));padding:12px;background:rgba(127,29,29,.9);border:1px solid rgba(248,113,113,.8);border-radius:8px;color:#fee2e2;font-size:14px;display:none}
    code{color:#93c5fd}
  </style>
</head>
<body>
  <main class="stage">
    <img id="video" src="/video" alt="ESP32 live video">
  </main>
  <div id="error" class="error"></div>
  <div class="panel">
    <button id="audioLeft">Audio left</button>
    <button id="audioRight" class="secondary">Audio right</button>
    <button id="audioStop" class="secondary">Stop audio</button>
    <a class="secondary" href="/health${tokenQuery}" target="_blank">Health</a>
    <span id="status" class="status">Video: <code>${ESP32_HOST}:${ESP32_HTTP_PORT}</code>, audio: <code>${ESP32_HOST}:${ESP32_AUDIO_PORT}</code></span>
  </div>
  <script>
    const statusEl = document.getElementById("status");
    const errorEl = document.getElementById("error");
    const videoEl = document.getElementById("video");
    let audioCtx = null;
    let ws = null;
    let nextAudioTime = 0;

    function wsBase() {
      return (location.protocol === "https:" ? "wss://" : "ws://") + location.host;
    }

    function setStatus(text) {
      statusEl.textContent = text;
    }

    function showError(text) {
      errorEl.textContent = text;
      errorEl.style.display = text ? "block" : "none";
    }

    async function checkStatus() {
      try {
        const response = await fetch("/status.json?cache=" + Date.now(), { cache: "no-store" });
        const status = await response.json();
        if (status.ok) {
          showError("");
          setStatus("ESP32 connected: " + status.target + " (" + status.elapsedMs + " ms)");
        } else {
          showError("ESP32 is not reachable from this PC: " + status.target + ". " + (status.error || ("HTTP " + status.statusCode)));
        }
      } catch (error) {
        showError("Proxy status check failed: " + error.message);
      }
    }

    videoEl.addEventListener("error", () => {
      showError("Video did not load. Check ESP32 IP, Wi-Fi network, and /health.");
      checkStatus();
    });

    function stopAudio() {
      if (ws) {
        const old = ws;
        ws = null;
        old.onclose = null;
        try { old.close(); } catch (_) {}
      }
      setStatus("Audio stopped");
    }

    async function startAudio(channel) {
      stopAudio();
      const AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) {
        setStatus("WebAudio is not supported in this browser");
        return;
      }
      if (!audioCtx) {
        try {
          audioCtx = new AC({ sampleRate: 16000 });
        } catch (_) {
          audioCtx = new AC();
        }
      }
      try { await audioCtx.resume(); } catch (_) {}

      nextAudioTime = audioCtx.currentTime + 0.05;
      ws = new WebSocket(wsBase() + "/audio.ws?ch=" + channel + "&shift=0&gain=4&cache=" + Date.now());
      ws.binaryType = "arraybuffer";
      ws.onopen = () => setStatus("Audio connected: " + channel);
      ws.onclose = () => {
        if (ws) setStatus("Audio disconnected");
        ws = null;
      };
      ws.onerror = () => setStatus("Audio WebSocket error");
      ws.onmessage = (event) => {
        const view = new DataView(event.data);
        const count = Math.floor(view.byteLength / 2);
        const buffer = audioCtx.createBuffer(1, count, 16000);
        const output = buffer.getChannelData(0);
        for (let i = 0; i < count; i++) {
          output[i] = Math.max(-1, Math.min(1, view.getInt16(i * 2, true) / 32768));
        }
        const source = audioCtx.createBufferSource();
        source.buffer = buffer;
        source.connect(audioCtx.destination);
        const now = audioCtx.currentTime;
        if (nextAudioTime < now + 0.02) nextAudioTime = now + 0.02;
        source.start(nextAudioTime);
        nextAudioTime += count / 16000;
        if (nextAudioTime > now + 0.25) nextAudioTime = now + 0.08;
      };
    }

    document.getElementById("audioLeft").addEventListener("click", () => startAudio("left"));
    document.getElementById("audioRight").addEventListener("click", () => startAudio("right"));
    document.getElementById("audioStop").addEventListener("click", stopAudio);
    checkStatus();
    setInterval(checkStatus, 10000);
  </script>
</body>
</html>`;
}

function handleRequest(req, res) {
  const parsedUrl = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  const path = parsedUrl.pathname;

  if (!proxyAuthorized(req, parsedUrl, res)) {
    sendUnauthorized(res);
    return;
  }

  if (req.method !== "GET") {
    sendText(res, 405, "Method not allowed\n");
    return;
  }

  if (path === "/") {
    sendText(res, 200, pageHtml(), "text/html; charset=utf-8");
    return;
  }

  if (path === "/status.json") {
    checkEsp32Health().then((status) => {
      sendText(res, status.ok ? 200 : 502, JSON.stringify(status, null, 2), "application/json; charset=utf-8");
    });
    return;
  }

  if (path === "/video") {
    proxyHttp(req, res, ESP32_HTTP_PORT, `/stream${parsedUrl.search}`, true);
    return;
  }

  if (path === "/capture" || path === "/health" || path === "/pins") {
    proxyHttp(req, res, ESP32_HTTP_PORT, `${path}${parsedUrl.search}`, true);
    return;
  }

  if (path === "/audio.wav" || path === "/audio/sample.wav" || path === "/audio/debug") {
    proxyHttp(req, res, ESP32_AUDIO_PORT, `${path}${parsedUrl.search}`, false);
    return;
  }

  sendText(res, 404, "Not found\n");
}

function rejectUpgrade(socket, status, message) {
  socket.write(`HTTP/1.1 ${status} ${message}\r\nConnection: close\r\n\r\n`);
  socket.destroy();
}

function handleUpgrade(req, socket, head) {
  const parsedUrl = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  if (!proxyAuthorized(req, parsedUrl, null)) {
    rejectUpgrade(socket, 401, "Unauthorized");
    return;
  }
  if (parsedUrl.pathname !== "/audio.ws") {
    rejectUpgrade(socket, 404, "Not Found");
    return;
  }

  const upstream = net.connect({ host: ESP32_HOST, port: ESP32_AUDIO_PORT }, () => {
    const lines = [
      `GET /audio.ws${parsedUrl.search} HTTP/1.1`,
      `Host: ${ESP32_HOST}:${ESP32_AUDIO_PORT}`,
      "Upgrade: websocket",
      "Connection: Upgrade",
      `Sec-WebSocket-Key: ${req.headers["sec-websocket-key"] || ""}`,
      `Sec-WebSocket-Version: ${req.headers["sec-websocket-version"] || "13"}`,
      `Origin: http://${ESP32_HOST}`,
    ];
    if (req.headers["sec-websocket-protocol"]) {
      lines.push(`Sec-WebSocket-Protocol: ${req.headers["sec-websocket-protocol"]}`);
    }
    upstream.write(`${lines.join("\r\n")}\r\n\r\n`);
    if (head && head.length) {
      upstream.write(head);
    }
    socket.pipe(upstream);
    upstream.pipe(socket);
  });

  upstream.on("error", () => rejectUpgrade(socket, 502, "Bad Gateway"));
  socket.on("error", () => upstream.destroy());
}

const server = http.createServer(handleRequest);
server.on("upgrade", handleUpgrade);

server.listen(PROXY_PORT, PROXY_HOST, () => {
  const localUrl = `http://localhost:${PROXY_PORT}/`;
  console.log(`ESP32 host: ${ESP32_HOST}`);
  console.log(`Video target: http://${ESP32_HOST}:${ESP32_HTTP_PORT}/stream`);
  console.log(`Audio target: ws://${ESP32_HOST}:${ESP32_AUDIO_PORT}/audio.ws`);
  console.log(`PC proxy: ${localUrl}`);
  if (PROXY_TOKEN) {
    console.log(`Proxy token is enabled. Open: ${localUrl}?token=${PROXY_TOKEN}`);
  }
  if (process.env.OPEN_BROWSER === "1") {
    exec(`start "" "${PROXY_TOKEN ? `${localUrl}?token=${encodeURIComponent(PROXY_TOKEN)}` : localUrl}"`);
  }
});

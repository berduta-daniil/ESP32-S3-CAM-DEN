"use strict";

const http = require("http");
const os = require("os");

const ESP32_HTTP_PORT = Number(process.env.ESP32_HTTP_PORT || 80);
const ESP32_AUTH_USER = process.env.ESP32_AUTH_USER || "cam";
const ESP32_AUTH_PASSWORD = process.env.ESP32_AUTH_PASSWORD || "1234";
const TIMEOUT_MS = Number(process.env.FIND_TIMEOUT_MS || 900);
const CONCURRENCY = Number(process.env.FIND_CONCURRENCY || 64);

const args = process.argv.slice(2);
const firstOnly = args.includes("--first");
const checkIndex = args.indexOf("--check");
const checkHost = checkIndex >= 0 ? args[checkIndex + 1] : "";

function basicAuthHeader() {
  return `Basic ${Buffer.from(`${ESP32_AUTH_USER}:${ESP32_AUTH_PASSWORD}`).toString("base64")}`;
}

function requestHealth(host) {
  return new Promise((resolve) => {
    const req = http.request(
      {
        host,
        port: ESP32_HTTP_PORT,
        path: "/health",
        method: "GET",
        timeout: TIMEOUT_MS,
        headers: {
          Authorization: basicAuthHeader(),
          Connection: "close",
        },
      },
      (res) => {
        let body = "";
        res.setEncoding("utf8");
        res.on("data", (chunk) => {
          if (body.length < 4096) body += chunk;
        });
        res.on("end", () => {
          const ok = res.statusCode === 200 && body.includes('"camera"') && body.includes('"sta_ip"');
          resolve({ host, ok, statusCode: res.statusCode, body });
        });
      }
    );

    req.on("timeout", () => req.destroy(new Error("timeout")));
    req.on("error", (error) => resolve({ host, ok: false, error: error.message }));
    req.end();
  });
}

function getCandidateHosts() {
  const hosts = new Set(["192.168.4.1"]);
  const interfaces = os.networkInterfaces();
  for (const entries of Object.values(interfaces)) {
    for (const item of entries || []) {
      if (item.family !== "IPv4" || item.internal || !item.address) continue;
      const parts = item.address.split(".");
      if (parts.length !== 4) continue;
      const base = `${parts[0]}.${parts[1]}.${parts[2]}.`;
      for (let i = 1; i <= 254; i++) {
        const host = `${base}${i}`;
        if (host !== item.address) hosts.add(host);
      }
    }
  }
  return Array.from(hosts);
}

async function scanHosts(hosts) {
  const found = [];
  let index = 0;

  async function worker() {
    while (index < hosts.length) {
      const host = hosts[index++];
      const result = await requestHealth(host);
      if (result.ok) {
        found.push(result);
        if (!firstOnly) {
          console.log(`FOUND ${host}`);
        }
      }
    }
  }

  await Promise.all(Array.from({ length: Math.min(CONCURRENCY, hosts.length) }, worker));
  return found;
}

async function main() {
  if (checkHost) {
    const result = await requestHealth(checkHost);
    if (result.ok) {
      console.log(`ESP32 reachable: http://${checkHost}:${ESP32_HTTP_PORT}/health`);
      return;
    }
    console.error(`ESP32 is not reachable at ${checkHost}:${ESP32_HTTP_PORT}`);
    if (result.statusCode) console.error(`HTTP status: ${result.statusCode}`);
    if (result.error) console.error(`Error: ${result.error}`);
    process.exitCode = 1;
    return;
  }

  if (!firstOnly) {
    console.log("Scanning local networks for ESP32 /health...");
  }
  const found = await scanHosts(getCandidateHosts());
  if (firstOnly) {
    if (found[0]) console.log(found[0].host);
    return;
  }
  if (!found.length) {
    console.log("No ESP32 board found.");
    console.log("Try direct AP mode: connect this PC to Wi-Fi ESP32-S3-CAM-DEN and use 192.168.4.1.");
    console.log("Or open Serial Monitor and copy the IP from: Router Wi-Fi connected. Open http://...");
    process.exitCode = 1;
    return;
  }
  console.log("");
  console.log("Use this IP in start-proxy.bat:");
  console.log(found[0].host);
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exitCode = 1;
});

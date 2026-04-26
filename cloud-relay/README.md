# Cloud relay

Node.js relay for the hybrid architecture:

- `ESP32 -> WSS relay` for board video and board microphone audio
- `GitHub Pages -> WSS relay` for playback in the browser
- `browser microphone -> WSS relay -> ESP32 speaker` for talkback
- `MQTT` stays separate and is used only for control/state

## Quick start

```powershell
cd cloud-relay
copy .env.example .env
npm install
npm start
```

By default the relay listens on port `8080`.

## Required secrets

- `DEVICE_ID`
- `DEVICE_TOKEN`
- `VIEWER_TOKEN`

`DEVICE_TOKEN` is flashed into the board through `include/relay_config.h`.

`VIEWER_TOKEN` is entered manually on the GitHub Pages site.

## Production

Recommended deployment:

- deploy `cloud-relay/` to a VPS or container host
- place `Caddy` or `Nginx` in front of it
- expose it as `https://relay.example.com`

The relay must be reachable over HTTPS/WSS so browser microphone access keeps
working on phones and modern desktop browsers.

# ESP32-S3-CAM-DEN PlatformIO camera

PlatformIO firmware for the board marked `ESP32-S3-SPK` / `N16R8` with an
`OV2640` camera. It serves a browser page with:

- single JPEG snapshot: `/capture`
- MJPEG stream: `/stream`
- low-latency microphone audio: `ws://<board-ip>:81/audio.ws`
- browser microphone to board speaker: `ws://<board-ip>:82/speaker.ws`
- microphone WAV fallback: `http://<board-ip>:81/audio.wav`
- optional cloud relay mode for GitHub Pages + WSS
- diagnostics: `/health` and `/pins`

## Project structure

- firmware: this root PlatformIO project
- GitHub Pages site: `github-pages/`
- cloud relay server: `cloud-relay/`

## Cloud mode architecture

For phone/Chrome microphone capture without a local PC, the browser must run in
a secure context (`HTTPS`) and talk to the board through a relay:

1. `ESP32 -> WSS relay`
2. `GitHub Pages -> WSS relay`
3. `browser microphone -> WSS relay -> ESP32 speaker`

This avoids the `http://192.168.x.x` microphone limitation in modern browsers.

## Enable cloud relay on ESP32

1. Copy `include/relay_config.example.h` to `include/relay_config.h`.
2. Fill:
   - `RELAY_HOST`
   - `RELAY_DEVICE_ID`
   - `RELAY_DEVICE_TOKEN`
3. Rebuild and upload.

The local AP/router web interface still remains available as a fallback.

## Build and upload

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

If upload does not start automatically, hold `BOOT`, press and release `RST`,
then release `BOOT` and run upload again.

## Open the camera page

By default the firmware starts a direct Wi-Fi access point:

- SSID: `ESP32-S3-CAM-DEN`
- Password: `12345678`
- URL: `http://192.168.4.1`
- Web login: `cam`
- Web password: `1234`

Connect your phone or computer to that Wi-Fi network and open the URL above.
The page auto-starts live video. On desktop it also auto-starts the low-latency
microphone stream. On phones, tap `Start live A/V` or one of the `Live` buttons
to start audio playback through the browser audio element fallback.

The microphone stream is served on a separate HTTP port so it can work while the
camera stream is open:

- AP live audio URL: `ws://192.168.4.1:81/audio.ws?ch=left&shift=0&gain=4`
- Router live audio URL: `ws://<router-ip>:81/audio.ws?ch=left&shift=0&gain=4`
- Browser talkback URL: `ws://<board-ip>:82/speaker.ws?gain=2`
- WAV fallback URL: `http://<board-ip>:81/audio.wav?ch=left&shift=0&gain=4`

If the microphone is silent or noisy, try:

- right L/R select: `ws://<board-ip>:81/audio.ws?ch=right&shift=0&gain=4`
- louder left channel: `ws://<board-ip>:81/audio.ws?ch=left&shift=0&gain=8`
- quieter left channel: `ws://<board-ip>:81/audio.ws?ch=left&shift=1&gain=4`
- finite 3-second recording: `http://<board-ip>:81/audio/sample.wav?ch=left&shift=0&gain=4&ms=3000`
- raw level diagnostics: `http://<board-ip>:81/audio/debug`

## Speaker and microphone tests

The board also has an NS4168 I2S speaker amplifier. The web page contains test
buttons:

- `Speaker tone` plays a 1 kHz tone through the connected speaker.
- `Browser mic -> speaker` sends the microphone of your phone/PC browser to the board speaker.
  Modern mobile/Chrome browsers allow this only from `https://` or `localhost`.
- `Download mic WAV` records a finite 3-second microphone sample for checking on
  a computer.
- `Mic->speaker left/right` sends the microphone to the speaker for 3 seconds at
  very low volume. Keep the speaker away from the microphone to avoid feedback.

Direct test endpoints:

- speaker tone: `http://<board-ip>/speaker/tone?freq=1000&ms=1200&amp=2500`
- quiet mic loopback: `http://<board-ip>/speaker/mic?ch=left&shift=4&gain=6&ms=3000`

## Deploy the cloud relay

The relay project is in `cloud-relay/`.

Quick local test:

```powershell
cd cloud-relay
copy .env.example .env
npm install
npm start
```

Production recommendation:

- deploy `cloud-relay/` to a VPS or container host
- place `Caddy` / `Nginx` in front of it
- expose it as `https://relay.example.com`

Example reverse proxy config is in `cloud-relay/Caddyfile.example`.

## Deploy the GitHub Pages site

The static site source is in `github-pages/`.

Recommended publish mode for this repository:

- push the firmware/relay code to `main`
- publish the site from the separate `gh-pages` branch root

This avoids requiring GitHub Actions `workflow` scope on the current token.

After opening the site:

1. enter `Relay URL`
2. enter `Device ID`
3. enter `Viewer token`
4. press `Connect`

Then:

- `Start A/V` starts board video + board audio
- `Start talkback` sends phone/PC microphone to the board speaker

## Optional router Wi-Fi

To connect the board to your router too:

1. Copy `include/wifi_config.example.h` to `include/wifi_config.h`.
2. Fill `WIFI_STA_SSID` and `WIFI_STA_PASSWORD`.
3. Rebuild and upload.

The direct access point remains enabled, so the board is still reachable at
`http://192.168.4.1` if router Wi-Fi fails.

## PSRAM note

This board is configured as `qio_qspi` because the tested module reports an
error when firmware is built for Octal PSRAM. On boot, Serial Monitor should
show a non-zero `psram` value in `Boot heap` / `Heap before camera`. If PSRAM is
still unavailable, the firmware falls back to internal DRAM and keeps the camera
at `QVGA JPEG, 1 fb`.

## Camera pins

The active preset is in `include/camera_pins.h`:

- XCLK: `GPIO33`
- SCCB SDA/SCL: `GPIO37` / `GPIO36`
- D0..D7: `GPIO7`, `GPIO5`, `GPIO4`, `GPIO6`, `GPIO8`, `GPIO42`, `GPIO48`, `GPIO47`
- VSYNC/HREF/PCLK: `GPIO35` / `GPIO34` / `GPIO41`

This pinout is for the ESP32-S3 speaker/camera N16R8 board family with OV2640.

## Microphone pins

The active preset is in `include/audio_pins.h`:

- PDM DATA: `GPIO38`
- PDM CLK: `GPIO39`
- L/R select: `GPIO40`

The seller diagram for this board marks `MSM261D3526H1CPM`, which is a PDM
digital microphone. The firmware uses ESP32-S3 PDM RX on `I2S0` and converts it
to 16 kHz mono PCM. The web page can play it as low-latency WebSocket/WebAudio
or as a buffered WAV fallback.

## Speaker pins

The active preset is in `include/speaker_pins.h`:

- PA control: `GPIO46`
- I2S LRCLK: `GPIO45`
- I2S BCLK: `GPIO10`
- I2S DOUT: `GPIO9`

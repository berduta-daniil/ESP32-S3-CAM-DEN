# ESP32-S3-CAM-DEN local camera

PlatformIO firmware for the `ESP32-S3-SPK` / `N16R8` board with an `OV2640`
camera and built-in PDM microphone.

The project is local-network only:

- video page and MJPEG stream: `http://<board-ip>/` and `http://<board-ip>/stream`
- microphone WebSocket audio: `ws://<board-ip>:81/audio.ws?ch=left&shift=0&gain=4`
- microphone WAV fallback: `http://<board-ip>:81/audio.wav?ch=left&shift=0&gain=4`
- diagnostics: `http://<board-ip>/health`, `http://<board-ip>/pins`,
  `http://<board-ip>:81/audio/debug`

Opening the board IP in a browser shows only the live video image immediately.
Audio stays on the same IP but a separate port, `81`.

## Build and upload

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

If upload does not start automatically, hold `BOOT`, press and release `RST`,
then release `BOOT` and run upload again.

## Open in browser

By default the firmware starts a direct Wi-Fi access point:

- SSID: `ESP32-S3-CAM-DEN`
- Password: `12345678`
- URL: `http://192.168.4.1`
- Web login: `cam`
- Web password: `1234`

Connect your phone or computer to that Wi-Fi network and open:

```text
http://192.168.4.1
```

If router Wi-Fi is configured, open the IP printed in Serial Monitor:

```text
Router Wi-Fi connected. Open http://<router-ip>
```

## Optional router Wi-Fi

To connect the board to your router too:

1. Copy `include/wifi_config.example.h` to `include/wifi_config.h`.
2. Fill `WIFI_STA_SSID` and `WIFI_STA_PASSWORD`.
3. Rebuild and upload.

The direct access point remains enabled, so the board is still reachable at
`http://192.168.4.1` if router Wi-Fi fails.

## Local audio URLs

The microphone stream is served on port `81` so it can work independently from
the video page on port `80`.

- AP audio: `ws://192.168.4.1:81/audio.ws?ch=left&shift=0&gain=4`
- Router audio: `ws://<router-ip>:81/audio.ws?ch=left&shift=0&gain=4`
- WAV fallback: `http://<board-ip>:81/audio.wav?ch=left&shift=0&gain=4`
- 3-second sample: `http://<board-ip>:81/audio/sample.wav?ch=left&shift=0&gain=4&ms=3000`

If the microphone is silent or noisy, try:

- right channel: `ws://<board-ip>:81/audio.ws?ch=right&shift=0&gain=4`
- louder left channel: `ws://<board-ip>:81/audio.ws?ch=left&shift=0&gain=8`
- quieter left channel: `ws://<board-ip>:81/audio.ws?ch=left&shift=1&gain=4`

## Pins

The active camera preset is in `include/camera_pins.h`:

- XCLK: `GPIO33`
- SCCB SDA/SCL: `GPIO37` / `GPIO36`
- D0..D7: `GPIO7`, `GPIO5`, `GPIO4`, `GPIO6`, `GPIO8`, `GPIO42`, `GPIO48`, `GPIO47`
- VSYNC/HREF/PCLK: `GPIO35` / `GPIO34` / `GPIO41`

The active microphone preset is in `include/audio_pins.h`:

- PDM DATA: `GPIO38`
- PDM CLK: `GPIO39`
- L/R select: `GPIO40`

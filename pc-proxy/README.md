# PC proxy for ESP32-S3-CAM-DEN

This proxy runs on a Windows PC in the same local network as the ESP32 board.
The browser opens one PC address, while the server forwards:

- `/video` -> `http://<ESP32-IP>:80/stream`
- `/audio.ws` -> `ws://<ESP32-IP>:81/audio.ws`
- `/audio.wav`, `/audio/sample.wav`, `/audio/debug` -> ESP32 audio port `81`

## Quick start

1. Install Node.js LTS if it is not installed.
2. Run `start-proxy.bat`.
3. The script tries to find the ESP32 automatically. If it asks for an IP,
   enter the board IP, for example `192.168.4.1`.
4. Open `http://localhost:8080/`.

The BAT file sets defaults:

```text
ESP32_HTTP_PORT=80
ESP32_AUDIO_PORT=81
PROXY_PORT=8080
ESP32_AUTH_USER=cam
ESP32_AUTH_PASSWORD=1234
```

## Find the board IP

Run:

```bat
find-board.bat
```

If the board is connected to the same router as the PC, the IP should look like
the PC/router subnet, for example `192.168.50.xxx`.

If the board is not connected to that router, connect the PC directly to:

```text
SSID: ESP32-S3-CAM-DEN
Password: 12345678
Board IP: 192.168.4.1
```

You can also open PlatformIO Serial Monitor and copy the IP from:

```text
Router Wi-Fi connected. Open http://...
```

## Internet access

Run:

```bat
start-proxy.bat
```

When it asks:

```text
Open video to internet via Cloudflare Tunnel? [y/N]:
```

enter:

```text
y
```

The script downloads `cloudflared.exe` into `pc-proxy\tools` if needed, starts
the local proxy, waits for Cloudflare to create the real public URL, and opens
it automatically. The URL will look similar to:

```text
https://random-words.trycloudflare.com/?token=esp32-12345-12345-12345
```

Use the exact URL printed by the script. Do not use `example.trycloudflare.com`;
that is only a placeholder.

Keep the BAT window open while streaming.

You can also expose the PC proxy with ngrok, Tailscale Funnel, or router port
forwarding to the PC port `8080`.

For public access, set a token before running:

```bat
set PROXY_TOKEN=change-this-password
start-proxy.bat
```

Then open:

```text
http://localhost:8080/?token=change-this-password
```

The browser keeps the token in a cookie so video and audio requests work from
the same page.

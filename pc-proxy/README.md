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

The script starts the internet tunnel automatically. It tries ngrok first,
using the saved ngrok configuration from a previous successful login.

For the first ngrok setup:

1. Create a free ngrok account.
2. Copy your token from `https://dashboard.ngrok.com/get-started/your-authtoken`.
3. Run once:

```bat
pc-proxy\tools\ngrok.exe config add-authtoken YOUR_TOKEN
```

After that, `start-proxy.bat` does not ask for the token again.

The script downloads `ngrok.exe` into `pc-proxy\tools` if needed, starts the
local proxy, and opens a public URL like:

```text
https://random-name.ngrok-free.app/?token=esp32-12345-12345-12345
```

If ngrok reports `ERR_NGROK_334`, an old ngrok tunnel is still online. The BAT
script now tries to stop old local `ngrok.exe` processes automatically before
starting a new tunnel. If the error remains, close old terminal windows or stop
the endpoint from the ngrok dashboard.

If no ngrok token is provided, the script tries Cloudflare Tunnel. If
Cloudflare times out, it tries a `localtunnel` fallback through `npx`.

The script opens the real public URL automatically. It will look similar to:

```text
https://random-words.trycloudflare.com/?token=esp32-12345-12345-12345
```

Use the exact URL printed by the script. Do not use `example.trycloudflare.com`;
that is only a placeholder.

If Cloudflare is blocked in the current network, the fallback URL may look like:

```text
https://random-name.loca.lt/?token=esp32-12345-12345-12345
```

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

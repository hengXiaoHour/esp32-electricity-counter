# ESP32-S3 6-Channel AC Electricity Counter

Monitors 6 AC circuits with current sensors, 1 voltage reference, RGB status LED, active buzzer alert, and a real-time WebSocket dashboard with OTA updates.

## Hardware Setup

| Component | Pins |
|---|---|
| CT sensors (ch1-6) | GPIO7, 5, 6, 8, 4, 2 |
| Voltage sensor | GPIO1 |
| Active buzzer | GPIO13 |
| RGB LED (WS2812) | GPIO48 |

Each channel has an independent monthly kWh limit. When a channel reaches 100% of its limit it trips (RED LED blink) and the buzzer rings N beeps (N = channel number, ch1 = 1 beep … ch6 = 6 beeps).

## Arduino IDE Setup

1. **Board:** Tools → Board → ESP32 Arduino → `ESP32S3 Dev Module`
2. **Partition Scheme:** Tools → Partition Scheme → `No FS 4MB (2MB APP with OTA)` — REQUIRED. The firmware (with the Firebase library) does not fit the default 1.25MB APP partition.
3. **Flash Size:** 4MB (matches the onboard XMC embedded flash)
4. **PSRAM:** Enabled (2MB PSRAM on this board)

## Required Libraries

Install via Arduino Library Manager:

| Library | Version |
|---|---|
| `Adafruit NeoPixel` | ≥1.15 |
| `ESP Async WebServer` | ≥3.11 |
| `AsyncTCP` | ≥1.1 |
| `Firebase Arduino Client Library for ESP8266 and ESP32` | ≥4.4.17 |

## Firebase Cloud Setup (Cloud dashboard mode)

The dashboard supports a **Cloud** mode over Firebase Realtime Database (reachable from anywhere, no port-forwarding), alongside the LAN **WebSocket** mode.

1. Create a Firebase project (e.g. `esp32-electricity-counter`) and a Realtime Database instance (us-central1).
2. Deploy the RTDB rules + hosting from this repo:
   ```bash
   firebase deploy
   ```
3. **Device credentials** — Firebase Console → Project Settings → Service Accounts → *Generate new private key*. Fill the values into `src/network/firebase_config.h` (copy from `src/network/firebase_config.example.h`; this file is gitignored).
4. **Google sign-in (admin)** — Firebase Console → Authentication → Get started → enable the **Google** sign-in provider, then add `heng.xiao.hour@gmail.com` to `frontend/config.js` -> `adminEmails` (the list here only drives the UI; enforce it in `database.rules.json` -> `isAdmin()` which is deployed with `firebase deploy`). The web `apiKey`/`authDomain` in `frontend/config.js` come from Project Settings → Your apps → Web app.
5. **Dashboard config** — copy `frontend/config.js` from `frontend/config.example.js` (gitignored) and verify `databaseURL` matches your RTDB instance.
6. Re-upload the firmware (Firebase starts automatically when WiFi connects) and open the dashboard: pick **Cloud** on the connect screen. Guests (who haven't signed in with the admin Google account) are **read-only** — commands, console, calibration, counters and OTA stay locked.

**Auth scope note:** admin gating is enforced by the RTDB security rules, so **Cloud** mode is always protected even if the dashboard UI is bypassed. **Local (WebSocket)** mode has no server-side auth on the ESP32, so the admin check there is UI-level only — treat Local as trusted-LAN-only. **Demo** mode is intentionally fully open.

Data flow: the device pushes its live snapshot to `/latest` every 1 s (service-account auth); the dashboard subscribes to it; commands are fire-and-forget pushes to `/commands` that the device polls and executes.

## First Boot — WiFi Setup

1. Power on the ESP32-S3
2. Connect to the **ESP32-Elec-Counter** WiFi AP (password: `configure123`)
3. Open any webpage — the captive portal shows the config form
4. Enter your WiFi SSID/password and click Save
5. The device reboots and connects. Find its IP on your router or check Serial output.

## Dashboard

The dashboard lives in `frontend/` and is deployed to **Firebase Hosting** (https://esp32-electricity-counter.web.app) — it is *not* served from the device:

```bash
firebase deploy --only hosting   # after changing frontend/ files
```

Three connection modes on the connect screen:
- **Cloud** — Firebase RTDB (`/latest` + `/commands`), works from anywhere
- **Local** — WebSocket directly to `ws://<esp32-ip>/ws` on your LAN
- **Demo** — mock data, no device required

## OTA Updates

Once connected to WiFi, you can upload new firmware over-the-air:
1. Arduino IDE → Select the ESP32 IP as a network port
2. Sketch → Upload

The RGB LED turns blue during OTA. The dashboard shows OTA progress.

## Architecture

The firmware runs on two FreeRTOS cores:

- **Core 0** — Networking: WiFi management, WebSocket server, Firebase RTDB bridge, OTA handler
- **Core 1** — Sensing: ADC sampling, power calculation, limit checking, buzzer alerts

Shared data between cores is protected by a mutex (`SemaphoreHandle_t`).

## Serial Monitor

- Baud: 115200
- Boot messages, connection status, and sensor readings are printed on startup
- Calibration values can be adjusted via the dashboard under the Calibration section

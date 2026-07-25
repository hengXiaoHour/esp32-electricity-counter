# ESP32-S3 6-Channel AC Electricity Counter

Monitors 6 AC circuits with current sensors, 1 voltage reference, 4 relay outputs, RGB status LED, and a real-time WebSocket dashboard with OTA updates.

## Hardware Setup

| Component | Pins |
|---|---|
| CT sensors (ch1-6) | GPIO2, 16, 4, 5, 6, 7 |
| Voltage sensor | GPIO1 |
| Relays (ch1-4) | GPIO43, 44, 13, 12 |
| RGB LED (WS2812) | GPIO48 |

Channels 1-4 have relays; channels 5-6 are monitoring-only. Relays are active-LOW by default (configurable in `src/config.h`).

## Arduino IDE Setup

1. **Board:** Tools → Board → ESP32 Arduino → `ESP32S3 Dev Module`
2. **Partition Scheme:** Tools → Partition Scheme → `Huge APP (3MB No OTA/1MB SPIFFS)` **or** `8M with spiffs (3MB APP/1.5MB SPIFFS)` for OTA
3. **Flash Size:** 16MB (if available) or 8MB
4. **PSRAM:** Enabled (if your board has PSRAM)

## Required Libraries

Install via Arduino Library Manager:

| Library | Version |
|---|---|
| `Adafruit NeoPixel` | ≥1.15 |
| `ESP Async WebServer` | ≥3.11 |
| `AsyncTCP` | ≥1.1 |

## First Boot — WiFi Setup

1. Power on the ESP32-S3
2. Connect to the **ESP32-Elec-Counter** WiFi AP (password: `configure123`)
3. Open any webpage — the captive portal shows the config form
4. Enter your WiFi SSID/password and click Save
5. The device reboots and connects. Find its IP on your router or check Serial output.

## Uploading Dashboard Web Files

The dashboard (HTML/CSS/JS in `data/`) must be uploaded to LittleFS:

### Using Arduino IDE + ESP32 Sketch Data Upload plugin:
1. Tools → ESP32 Sketch Data Upload
2. Select the `data/` folder
3. Upload

### Using `arduino-cli` + `mklittlefs`:
```bash
# Build and upload LittleFS image
mklittlefs -c data/ -p 256 -b 4096 -s 0x180000 littlefs.bin
esptool.py write_flash 0x2D0000 littlefs.bin  # adjust address for your partition
```

## OTA Updates

Once connected to WiFi, you can upload new firmware over-the-air:
1. Arduino IDE → Select the ESP32 IP as a network port
2. Sketch → Upload

The RGB LED turns blue during OTA. The dashboard shows OTA progress.

## Architecture

The firmware runs on two FreeRTOS cores:

- **Core 0** — Networking: WiFi management, WebSocket server, OTA handler
- **Core 1** — Sensing: ADC sampling, power calculation, limit checking, relay control

Shared data between cores is protected by a mutex (`SemaphoreHandle_t`).

## Serial Monitor

- Baud: 115200
- Boot messages, connection status, and sensor readings are printed on startup
- Calibration values can be adjusted via the dashboard under the Calibration section

# AGENTS.md — ESP32-S3 Electricity Counter

## Project Summary
ESP32-S3 firmware (Arduino C++) for monitoring 6 AC circuits with SCT-013-100 current sensors and ZMPT101B voltage sensor. Dual-core FreeRTOS: Core 0 handles networking (WiFi, WebSocket dashboard, OTA), Core 1 handles sensing (ADC sampling, power calculation, limit checking, buzzer alerts). WebSocket dashboard served from LittleFS.

## Key Architecture
- Main sketch: `esp32-electricity-counter.ino` at project root
- All source modules in `src/` with snake_case naming
- Shared `SystemData` protected by `SemaphoreHandle_t` mutex
- NVS (Preferences) for WiFi creds, channel configs, calibration

## Pin Mapping
- CT sensors: GPIO7,5,6,8,4,2
- Voltage: GPIO1
- Active buzzer: GPIO13 (limit-trip alert, N beeps = channel no.)
- RGB LED: GPIO48 (WS2812, R/G swapped)

## Required Libraries
- Adafruit NeoPixel
- ESP Async WebServer
- AsyncTCP

## Hosting Model (2026-07-25)
- **UI is hosted externally** on PC/phone (opened via browser from local file or `python3 -m http.server`)
- ESP32 only runs the **WebSocket server** (`ws://<esp32-ip>/ws`) — no LittleFS file serving
- Dashboard starts with a **connection panel** where the user enters the ESP32's IP address
- Last-used IP is saved in `localStorage` for auto-reconnect on page reload
- The ESP32 firmware should only serve the WebSocket endpoint, not HTTP static files

## Patches Applied (2026-07-24)
- AsyncTCP `status()` made const to fix compile error with ESPAsyncWebServer

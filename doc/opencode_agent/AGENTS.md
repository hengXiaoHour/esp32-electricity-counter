# AGENTS.md — ESP32-S3 Electricity Counter

## Project Summary
ESP32-S3 firmware (Arduino C++) for monitoring 6 AC circuits with SCT-013-100 current sensors and ZMPT101B voltage sensor. Dual-core FreeRTOS: Core 0 handles networking (WiFi, WebSocket dashboard, OTA), Core 1 handles sensing (ADC sampling, power calculation, limit checking, relay control). WebSocket dashboard served from LittleFS.

## Key Architecture
- Main sketch: `esp32-electricity-counter.ino` at project root
- All source modules in `src/` with snake_case naming
- Shared `SystemData` protected by `SemaphoreHandle_t` mutex
- NVS (Preferences) for WiFi creds, channel configs, calibration

## Pin Mapping
- CT sensors: GPIO2,16,4,5,6,7
- Voltage: GPIO1
- Relays: GPIO43,44,13,12 (active-LOW, channels 1-4 only)
- RGB LED: GPIO48 (WS2812, R/G swapped)

## Required Libraries
- Adafruit NeoPixel
- ESP Async WebServer
- AsyncTCP

## Patches Applied (2026-07-24)
- AsyncTCP `status()` made const to fix compile error with ESPAsyncWebServer

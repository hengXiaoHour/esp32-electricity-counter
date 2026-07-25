# Research: ESP32-S3 6-Channel AC Electricity Counter

## Current State

Empty project — git initialized, architecture locked in `doc/ARCHITECTURE.md`. Build prompt at `doc/esp32s3-electricity-counter-prompt.md`.

## Requirements Summary

**Hardware:**
- ESP32-S3 (dual-core), 6× SCT-013-100 current sensors (GPIO2,16,4,5,6,7), 1× ZMPT101B voltage sensor (GPIO1), 4× relay module (GPIO43,44,13,12, active-LOW), 1× WS2812 RGB LED (GPIO48, R/G swapped)
- Channels 1-4 relay-controllable, channels 5-6 monitoring-only

**Software Architecture:**
- Arduino IDE / Arduino framework (not ESP-IDF, not PlatformIO)
- Dual-core FreeRTOS: Core 0 = networking/UI, Core 1 = sensor sampling/math
- Shared data protected by `SemaphoreHandle_t`
- NVS (`Preferences`) for per-channel config (names, limits) and WiFi creds
- LittleFS for dashboard web files
- AsyncWebServer + AsyncWebSocket for dashboard
- ArduinoOTA for OTA updates
- Fallback AP + captive portal if no WiFi credentials stored

**Dashboard:**
- Single-page mobile-responsive HTML/CSS/JS served from LittleFS
- Real-time WebSocket updates (no polling)
- Per-channel: name, current, power, kWh, status, relay state, limit, reset button
- Editable settings panel
- Event log

**Key Constraints:**
- No `delay()` in loops
- Non-blocking throughout
- Relay active-LOW (configurable via `#define`)
- WS2812 R/G channel swap wrapper
- Calibration constants in NVS

## Libraries Required

| Library | Purpose |
|---|---|
| `ESPAsyncWebServer` | Async HTTP + WebSocket server |
| `AsyncTCP` | Required by ESPAsyncWebServer on ESP32 |
| `ArduinoOTA` | OTA firmware updates |
| `Adafruit NeoPixel` | WS2812 RGB LED |
| `Preferences` | Built-in, NVS storage |
| `LittleFS` | Built-in, SPIFFS alternative |
| `WiFi` | Built-in |
| `AsyncPing` (optional) | Network diagnostics |

## Pin Validation (ESP32-S3)

- GPIO1-7: All are valid ADC1 channels on ESP32-S3 ✅
- GPIO43,44: Valid GPIOs ✅
- GPIO48: Valid for WS2812 (RMT peripheral) ✅
- GPIO12,13: Valid GPIOs ✅

## Approach Options

**Option A — Single .ino with tab-split headers/cpp** (recommended)
- `main.ino` as entry point, split logic into `.h`/`.cpp` files per module
- Works cleanly with Arduino IDE multi-tab sketches
- Easy to compile and flash

**Option B — All-in-one .ino**
- Everything in a single file — not recommended per the prompt (readable split required)

**Chosen: Option A**

## Risks

1. **ADC noise**: ESP32-S3 ADC is known to be noisy — calibration constants essential
2. **FreeRTOS stack sizing**: Need to allocate enough stack for WebSocket JSON serialization on Core 0
3. **LittleFS partition**: Must specify correct partition scheme in Arduino IDE
4. **6 current channels + 1 voltage**: ADC sampling loop needs to be fast enough — RTOS task priority tuning
5. **R/G swap confusion**: Must document clearly so nobody "fixes" it later

## Open Questions

- Desired warning threshold % (currently assuming 90% from prompt)
- Calibration default values for SCT-013-100 and ZMPT101B (using typical values from datasheets)
- WebSocket update interval (suggesting 500ms)

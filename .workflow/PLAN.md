# Plan: ESP32-S3 6-Channel AC Electricity Counter

## Files to Create

```
src/
├── config.h                       # Pin defines, constants, calibration defaults
├── main.ino                       # Entry point, FreeRTOS tasks, shared data
├── sensor/
│   ├── current_sensor.h
│   └── current_sensor.cpp         # 6-ch ADC sampling, RMS calculation
├── sensor/
│   ├── voltage_sensor.h
│   └── voltage_sensor.cpp         # ADC sampling, RMS, calibration
├── core/
│   ├── power_calculator.h
│   └── power_calculator.cpp       # Real power, apparent, PF, kWh
├── core/
│   ├── relay_controller.h
│   └── relay_controller.cpp       # Relay switching, active-LOW
├── core/
│   ├── limit_manager.h
│   └── limit_manager.cpp          # Warning/trip logic, event log
├── network/
│   ├── wifi_manager.h
│   └── wifi_manager.cpp           # WiFi + fallback AP + captive portal
├── network/
│   ├── websocket_server.h
│   └── websocket_server.cpp       # AsyncWebSocket, JSON, commands
├── network/
│   ├── ota_handler.h
│   └── ota_handler.cpp            # ArduinoOTA setup
├── ui/
│   ├── status_led.h
│   └── status_led.cpp             # WS2812 with R/G swap
├── utils/
│   ├── nvs_manager.h
│   └── nvs_manager.cpp            # Preferences wrapper
data/
├── index.html                     # Dashboard HTML
├── style.css                      # Dashboard styles
└── script.js                      # WebSocket client + UI
```

## Implementation Steps

### Step 1: Config + NVS Manager
- `src/config.h` — all pin `#define`s, constants (warning threshold, ADC params, calibration defaults), relay active-LOW flag
- `src/utils/nvs_manager.h/.cpp` — Preferences wrapper: save/load channel configs (name, limit), WiFi creds, calibration values
- **Verification:** Compile check

### Step 2: Status LED
- `src/ui/status_led.h/.cpp` — WS2812 wrapper, `setStatusColor()` with R/G swap, blink patterns for each state (green, yellow, red, blue)
- **Verification:** Compile check

### Step 3: Sensors (Current + Voltage)
- `src/sensor/current_sensor.h/.cpp` — ADC init, sample all 6 channels, compute RMS current
- `src/sensor/voltage_sensor.h/.cpp` — ADC init, sample, compute RMS voltage with calibration
- **Verification:** Compile check

### Step 4: Power Calculator
- `src/core/power_calculator.h/.cpp` — Real power (V×I), apparent power (V_rms × I_rms), power factor, accumulated kWh
- **Verification:** Compile check

### Step 5: Relay Controller + Limit Manager
- `src/core/relay_controller.h/.cpp` — Relay init, on/off with active-LOW config, status readback
- `src/core/limit_manager.h/.cpp` — Warning (≥90%) / trip (≥100%) logic, event log ring buffer, per-channel state machine
- **Verification:** Compile check

### Step 6: WiFi Manager
- `src/network/wifi_manager.h/.cpp` — Connect using stored creds, fallback AP + captive portal/DNS, retry/backoff, connection callback
- **Verification:** Compile check

### Step 7: WebSocket Server + OTA
- `src/network/websocket_server.h/.cpp` — AsyncWebServer + AsyncWebSocket, JSON serialization of shared data, command parsing (set limit, set name, reset relay)
- `src/network/ota_handler.h/.cpp` — ArduinoOTA setup, progress callback, LED status integration
- **Verification:** Compile check

### Step 8: main.ino
- Shared data struct with `SemaphoreHandle_t`
- Core 0 task: networking (WiFi, WebSocket, OTA)
- Core 1 task: sensor sampling + math + limit checking
- `setup()` and `loop()` — FreeRTOS task creation, no `delay()`
- **Verification:** Compile check

### Step 9: Dashboard Web Files
- `data/index.html` — mobile-responsive single-page layout with per-channel cards, settings panel, event log
- `data/style.css` — clean modern styling
- `data/script.js` — WebSocket client, live updates, editable fields, relay reset buttons
- **Verification:** Manual review (visual correctness)

### Step 10: README + Final Integration
- Update `README.md` with setup instructions: board selection, partition scheme, library installation, first-boot WiFi provisioning
- **Verification:** Full compile check

## Test Strategy

Arduino C++ firmware — no automated test framework available. Verification = compile against ESP32-S3 target using Arduino IDE/CLI. Each step's verification confirms no syntax or type errors.

If `arduino-cli` is available: `arduino-cli compile --fqbn esp32:esp32:esp32s3 src/main.ino`

## Rollback

Each step is a git commit. If something breaks, `git revert <commit>` to undo.

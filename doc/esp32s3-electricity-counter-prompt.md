# Build Prompt — ESP32-S3 6-Channel AC Electricity Counter System

Copy everything below into OpenCode (DeepSeek V4 Flash) as your task prompt.

---

## Project Goal

Build a complete Arduino IDE (C++) firmware project for an **ESP32-S3** that monitors AC electricity consumption on 6 independent circuits, controls relays per circuit, shows system status on the onboard RGB LED, serves a real-time WebSocket dashboard, and supports OTA updates.

## Hardware Specification

**MCU:** ESP32-S3 (dual-core, Core 0 and Core 1 available)

**Current sensing (6 channels):**
- Sensor: SCT-013-100 (100A / 1V, 1% accuracy), non-invasive split-core CT clamp, analog output
- Pins: GPIO2, GPIO16, GPIO4, GPIO5, GPIO6, GPIO7 (channels 1–6 respectively)
- These are AC current sensors producing a sine wave centered at ~1.65V (mid-supply bias) — need burden resistor assumed already integrated in module, RMS calculation required via sampling.

**Voltage sensing (1 channel, shared reference for all 6 circuits):**
- Sensor: ZMPT101B voltage transformer module
- Pin: GPIO1
- Also analog, AC sine wave, needs RMS + calibration + phase alignment considerations with current channels for real power (W) calculation, not just apparent power.

**Relay outputs (4-channel relay module):**
- Pins: GPIO43, GPIO44, GPIO13, GPIO12
- Active-LOW or active-HIGH — make this a `#define` config flag (assume active-LOW typical relay module, but flag it clearly as configurable)
- These relays are used to cut power to a channel when that channel's configurable current/power limit is hit

**Onboard RGB LED (debug/status indicator):**
- Pin: GPIO48 (WS2812/NeoPixel type, single addressable LED)
- IMPORTANT: On this specific board, the red and green channels are physically swapped. Implement a `setStatusColor(r, g, b)` wrapper function that swaps R and G before writing to the LED, so all higher-level status code can use normal RGB naming (i.e., call `setStatusColor(0,255,0)` for "green" and it will actually be sent as `(255,0,0)` to the hardware). Add a code comment explaining this swap clearly so it's not "fixed" accidentally later.
- Status behavior:
  - Solid GREEN = Wi-Fi connected successfully, system nominal, all channels under their limits
  - Blinking YELLOW = one or more channels' consumption is approaching (e.g. ≥90%, make threshold a `#define`) its configured limit
  - (Add sensible additional states the AI should design: e.g. solid RED = Wi-Fi disconnected/AP fallback mode, blinking RED = a channel tripped its relay due to overshoot, solid BLUE = OTA update in progress/booting. Document all states in a comment block and in the dashboard.)

## Software Architecture Requirements

**Dual-core task split (must use FreeRTOS tasks pinned explicitly):**
- **Core 0:** Networking + WebSocket server + dashboard web server + OTA handling + JSON serialization of live data. This core must stay responsive for the UI regardless of sensor sampling load.
- **Core 1:** Real-time sensor sampling and math — ADC sampling of all 6 CT channels + 1 voltage channel, RMS current calculation, RMS voltage calculation, real power (W), apparent power (VA), power factor, accumulated energy (kWh) per channel, and limit-checking/relay-trip logic. Results are written to a thread-safe shared data structure (use a mutex/semaphore, e.g. FreeRTOS `SemaphoreHandle_t`) that Core 0 reads from to push to the dashboard.

**Per-channel configuration:**
- Each of the 6 channels needs its own configurable limit (current in Amps and/or power in Watts — implement power-based limiting using V×I since a shared voltage sensor is present, but also expose raw current as a fallback metric)
- Limits should be stored in non-volatile storage (`Preferences`/NVS) so they persist across reboots and are editable from the dashboard, not hardcoded
- Each channel needs a name/label field (also editable, also persisted)

**Relay + alert logic:**
1. Continuously compute RMS current/power per channel on Core 1
2. If a channel crosses the "warning" threshold (e.g. 90% of limit) → trigger blinking yellow LED, push a "warning" status for that channel to the dashboard, log the event (timestamp + channel + reading) — but do NOT cut power yet
3. If a channel crosses 100% of its configured limit → immediately switch off that channel's relay, set blinking RED LED, log the trip event with timestamp/channel/reading, and push an "tripped" alert to the dashboard
4. Provide a manual "reset/re-enable" control per channel on the dashboard to re-close the relay after a trip (don't auto-reclose — require explicit user action for safety)
5. All logic must map correctly: 6 current channels but only 4 relays — clarify/document which 4 of the 6 channels are relay-controllable (assume channels 1–4 map to the 4 relays unless told otherwise) and which 2 are monitoring-only. **Ask me to confirm this mapping if ambiguous rather than guessing silently.**

**Calibration:**
- Implement calibration constants as `#define` or NVS-stored values for both SCT-013-100 (current) and ZMPT101B (voltage) so they can be tuned against a real multimeter/clamp meter reading without recompiling logic (constants only)
- Include a commented-out example calibration routine/serial command for future tuning

**WebSocket Dashboard (served from ESP32-S3 itself, Core 0):**
- Use `ESPAsyncWebServer` + `AsyncWebSocket` (or an equivalent modern async library — pick one and justify briefly)
- Serve a clean, modern, mobile-responsive single-page dashboard (HTML/CSS/JS embedded via `LittleFS` or `PROGMEM`, your choice — prefer LittleFS for maintainability)
- Dashboard must show, live, updating over WebSocket (no polling):
  - Per-channel: name, current (A), power (W), energy accumulated (kWh), status (OK/Warning/Tripped), relay state, limit setting (editable)
  - Voltage reading
  - Wi-Fi status/signal strength
  - System uptime
  - A visual status matching the onboard RGB LED state (so status is visible even without seeing the physical LED)
  - Event log (last N warning/trip events with timestamps)
- Include editable settings panel: per-channel limits, per-channel names, warning threshold %, relay reset buttons

**OTA:**
- Implement `ArduinoOTA` (or `ElegantOTA` over the same async web server if that's cleaner given the dashboard is already async — pick the one that integrates best and explain the choice) so firmware can be updated over Wi-Fi without a USB cable
- Show OTA progress on serial and reflect an "updating" status on the RGB LED (e.g. solid blue) and dashboard

**Wi-Fi:**
- Connect to Wi-Fi using stored credentials (NVS/`Preferences`), with a fallback AP + captive portal (or simple config page) mode if no credentials are stored or connection fails, so the device is never unreachable
- Retry/backoff logic; reflect connection state on the LED per the states above

## Deliverables Requested From You (the AI)

1. Full PlatformIO-free, Arduino-IDE-compatible `.ino` main file (or clearly split `.ino` + header/source files if that's cleaner — Arduino IDE supports multi-tab sketches)
2. List of required Arduino Library Manager libraries with exact names to install
3. `frontend/` folder contents (dashboard HTML/CSS/JS) if you go that route
4. Clear pin/config `#define` block at the top of the code summarizing all pin assignments from this spec, so it's a single place to adjust hardware mapping
5. Brief setup instructions: board selection in Arduino IDE, partition scheme needed for OTA + LittleFS, and first-boot Wi-Fi provisioning steps
6. Explicitly flag any assumptions you make (e.g., relay active-HIGH/LOW, channel-to-relay mapping, calibration defaults) rather than silently guessing

## Constraints

- Arduino IDE / Arduino framework only (not ESP-IDF, not PlatformIO project files)
- Must actually compile for ESP32-S3 target (mind ADC pin validity — verify GPIO1–7 are valid ADC-capable pins on ESP32-S3 and flag it if any aren't, since some GPIOs in that range have restrictions)
- Prioritize code that's readable and split into logical sections/files over a single giant monolithic file
- Non-blocking code throughout — no `delay()` in loops handling sensors or networking

---

**End of prompt.** Paste this whole document into OpenCode with DeepSeek V4 Flash as the model, and let it generate the project.

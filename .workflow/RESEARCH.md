# Research: Web UI Live Console Terminal

## Current State

### Firmware
- **Serial commands**: `handleSerialCommand()` in `.ino:342-671` — 24 commands, all output via `Serial.printf()`, no capture possible
- **Web commands**: `processCommand()` in `command_processor.cpp` — 13 commands, returns `bool`, no text output
- **WebSocket**: `ws->textAll()` sends snapshots every 150ms; `client->text()` available but unused
- **Firebase**: writes to `/latest` (1s), polls `/commands` (1s), can write to any node
- **SystemData**: ~4.1KB struct, no console field

### Web UI
- `sendCommand(obj)` — single gateway routing Cloud/Local/Demo
- `updateDashboard(data)` — single receive sink for all data
- Settings tab: 4 panels (Connection, Notifications, Calibration, About)
- Dark theme, `--font-mono` already defined, no terminal styles

## Key Insight

Two disjoint command systems exist today. The web console needs a unified path that:
1. Accepts a text line from the UI
2. Executes it on the ESP32
3. Captures the text response
4. Sends it back through the same transport (WS or Firebase)

## Constraints
- `test led` blocks 8s with `delay()` — can't run in AsyncTCP context
- `nvs_debug` is destructive (writes test values to calibration NVS)
- `reboot`/`setwifi connect` call `ESP.restart()`
- Output contains `"` and `\n` — must be JSON-escaped

## Approach

### Firmware
1. Extract console output logic from `handleSerialCommand()` into a new `ConsoleHandler` class
2. Add `processConsoleCommand(const String &line, String &response)` to handle arbitrary text commands
3. Add a `console` command to `processCommand()` that delegates to the console handler with response capture
4. WebSocket: send response via `client->text()` to requesting client
5. Firebase: write response to `/console/` node, then delete command

### Web UI
1. New "Device Console" panel in Settings tab (between Calibration and About)
2. Terminal-style UI: scrollable output div + input row
3. Send via existing `sendCommand({cmd:'console', line:'...'})`
4. Receive via new `type:'console'` message handler

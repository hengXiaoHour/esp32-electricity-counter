# Research: Fix Dashboard Bugs + Red Dark Theme

## Current State

Three files in `data/`:
- `index.html` (97 lines) — contains inline `<style>` and `<script>` blocks that duplicate `style.css` and `script.js` verbatim. Does NOT link to external files.
- `style.css` (50 lines) — navy-blue theme (`#1a1a2e` bg, `#00d4ff` accent), thick 4px left-border cards, no blink animations.
- `script.js` (129 lines) — WebSocket client with `updateDashboard()`, `setLimit()`, `resetRelay()`, `sendCal()`. Single global `currCal`.

### Current JSON Payload Shape (inferred)
```json
{
  "v": 230.0, "wifi": true, "ap": false, "uptime": 3600, "ota": false,
  "voltageCalibration": 260, "currentCalibration": 100,
  "ch": [{"n":"Ch1","s":0,"r":true,"a":5.2,"w":1196,"va":1200,"pf":0.997,"kwh":123.456,"cl":10,"pl":2000}, ...],
  "events": [{"t":1234567890,"c":0,"s":2,"m":"message"}]
}
```

### Architecture
- Project root: `esp32-electricity-counter.ino` (Arduino C++)
- `src/` — C++ modules (sensors, core, network, ui, utils)
- `data/` — LittleFS web dashboard (HTML/CSS/JS)
- NVS (Preferences) for WiFi creds, channel configs, calibration

## Requirements (from prompt)

1. **Fix file duplication** — index.html links to style.css + script.js, no inline code
2. **hasRelay support** — channels 1-4 have relays, 5-6 are monitor-only. UI must hide relay indicator + reset button for monitor-only channels; show "Monitoring Only" tag
3. **Per-channel current calibration** — 6 separate values instead of 1 global. Keep voltage calibration global (single ZMPT101B). Each sends `{cmd:'set_current_cal', ch:i, val:x}`
4. **Blinking states** — CSS `@keyframes` pulse/blink applied via class toggle (`.blink-yellow`, `.blink-red`) on LED dot AND channel status badges
5. **OTA status section** — panel/banner that appears during OTA with progress percentage and firmware version
6. **Theme redesign** — Dark panel with red accent:
   - Near-black bg (`#0a0a0d`–`#121215`)
   - Brand red accent (`#e63946`) for header/buttons/borders
   - Green (`#2ecc71`) for OK status (semantic, distinct from brand red)
   - Amber for warning, bright red for tripped
   - Monospace for all numeric readings
   - 1px border cards with subtle shadow, hover states
   - Smooth transitions (150-200ms)
   - Keep responsive grid (`auto-fill`/`auto-fit`, `minmax(280px,1fr)`)

## Approach

- Clean rewrite of all 3 files (preserving functionality)
- Add `hasRelay: bool` per channel to payload
- Replace single `currentCalibration: number` with `currentCalibration: number[]` (array of 6)
- Add `otaProgress?: number` and `firmwareVersion?: string` for OTA panel
- Add `mono` class in HTML for monospace readings; CSS targets `.mono`
- No frameworks, no build step

## Risks
- Must not break existing limit-setting, reset, event log, uptime/wifi display
- Inline JS from index.html currently references `sendCal('set_voltage_cal','voltCal')` — this function signature changes
- Calibration rows must be populated dynamically (6 channels) — need to ensure they initialize only once
- Mobile responsive must survive the restyle

## Open Questions
- Confirm: channels 1-4 = hasRelay true, 5-6 = hasRelay false? (Assumed from prompt: relays on GPIO43,44,13,12 = 4 channels)
- The `off` class currently triggers on `!ch.r` (relay off) — after adding `hasRelay`, should `off` only apply to relay channels that are off, or stay as-is for monitor-only too?

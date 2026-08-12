# Plan: Expose LPF Alpha as a Dashboard Command

## Goal
Make the per-channel single-pole EMA LPF tunable from the dashboard calibration
panel via a new `set_lpf` command (mirroring `set_current_cal` / `set_noise_floor`).

## Files to Modify
- `src/network/command_processor.cpp` — add `set_lpf` branch (persist NVS + live `powerCalc`), update the comment header.
- `src/network/firebase_bridge.cpp` — add `"lpfAlpha":[...]` to `buildSystemJson()` so the dashboard can display current values.
- `frontend/script.js` — normalize `lpfAlpha` in `normalizeSnapshot`; add LPF row in calibration rows builder; `sendLpfAlpha(idx)`; sync `lpf_${i}` field; demo mock `lpfAlpha`; reset-NVS userSet cleanup.
- (no CSS/HTML changes needed — reuses the existing `cal-param-row` markup)

## Implementation Steps
1. Firmware `set_lpf` branch:
   - Files: `src/network/command_processor.cpp`
   - Verification: reads `"ch":` + `"val":`, validates `0<=ch<NUM_CHANNELS`, calls `powerCalc->setLpfAlpha(ch, val)` (already clamps 0.01–1.0) + `nvs->saveLpfAlpha(ch, val)`, sets `handled=true`.
2. Firmware JSON:
   - Files: `src/network/firebase_bridge.cpp`
   - Verification: emit `"lpfAlpha":[a0..a5]` with 2 decimals between `currentCalibration` and `rmsSamples`.
3. Frontend:
   - `normalizeSnapshot`: `if (val.lpfAlpha) norm.lpfAlpha = toArray(val.lpfAlpha);`
   - Calibration rows: add `<div class="cal-param-row"><label>LPF Alpha:</label><input id="lpf_${idx}" step="0.01" min="0.01" max="1" value="${lpf}"><button onclick="sendLpfAlpha(${idx})">Set</button></div>` (default 1).
   - `sendLpfAlpha(idx)`: parse, `delete dataset.userSet`, `sendCommand({cmd:'set_lpf', ch:idx, val})`, toast.
   - Sync: `data.lpfAlpha.forEach((v,i)=>syncField('lpf_'+i, v, 2))`.
   - Demo mock: add `lpfAlpha: [1,1,1,1,1,1]`.
   - `handleResetNvs`: clear `lpf_${i}` userSet.
   - Verification: `node --check frontend/script.js`.

## Test Strategy
- `node --check` for JS.
- Compile firmware with arduino-cli if available (`arduino-cli compile`) else PlatformIO/make per project convention.
- Browser smoke: demo shows LPF rows; guest can't see them (admin-only panel).

## Rollback
- Revert `set_lpf` branch + JSON field. Old firmware ignores unknown commands; old dashboard ignores `lpfAlpha`.

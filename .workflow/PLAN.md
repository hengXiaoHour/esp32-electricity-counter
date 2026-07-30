# Plan: Replace LPF with RMS Samples Tunable

## Summary
Remove per-channel LPF from ESP32 + UI. Replace with global `rmsSamples` tunable that controls how many ADC samples are used per RMS calculation (fewer = faster/noisier, more = slower/smoother).

## Files to Modify
1. `src/config.h` — rename `RMS_SAMPLES` → `MAX_RMS_SAMPLES`, add `rmsSamples` to `SystemData`
2. `src/core/power_calculator.h` — runtime `rmsSamples` member, `setRmsSamples()` method
3. `src/core/power_calculator.cpp` — use `rmsSamples` in loops, default 1000
4. `esp32-electricity-counter.ino` — sync `rmsSamples` to shared data
5. `src/network/websocket_server.cpp` — add `set_rms_samples` command, broadcast `rmsSamples`, remove `lpfAlpha`
6. `data/script.js` — remove LPF UI, add RMS Samples UI
7. `test_full_ui.py` — update for new controls

## Implementation Steps
1. `config.h` — MAX_RMS_SAMPLES + SystemData field
2. `power_calculator.h` — runtime member + setter
3. `power_calculator.cpp` — use rmsSamples in all loops
4. `.ino` — sync to shared data
5. `websocket_server.cpp` — command + JSON changes
6. `script.js` — UI changes
7. Build + Playwright test

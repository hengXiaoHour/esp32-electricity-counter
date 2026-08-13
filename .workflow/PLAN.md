# Plan: Per-Channel Calibration Reset Button

## Goal
Add a small "Reset" button to each channel's row in the System Calibration
panel that resets ONLY that channel's NVS + live calibration to defaults
(currentCal=100, noiseFloor=0, lpfAlpha=1), leaving all other channels and the
voltage/rms settings untouched.

## Files to Modify

### Firmware
- `src/network/command_processor.cpp` — add a `reset_ch_cal` branch (single-
  channel calibration reset mirroring the loop body of `reset_nvs_defaults`).
- `src/network/console_handler.cpp` — optional parity: skip (UI-only feature).

### Frontend
- `frontend/script.js` — add `sendResetChannelCal(idx)`; add a Reset button to
  each calibration collapse-body row; clear that channel's `dataset.userSet`
  so fresh values repopulate from the ESP32.
- `frontend/index.html` — no change (rows are built dynamically).
- `frontend/style.css` — no new CSS needed (reuses `.btn-sm`; maybe a danger
  variant color handled inline).

## Implementation Steps

1. **Firmware `reset_ch_cal` branch**
   - Files: `src/network/command_processor.cpp`
   - Parse `"ch":N`, accept only `0<=N<NUM_CHANNELS`.
   - `nvs->saveChannelCurrentCal(ch, DEFAULT_CURRENT_CALIBRATION);`
     `nvs->saveNoiseFloor(ch, 0.0f); nvs->saveLpfAlpha(ch, 1.0f);`
   - Live: `powerCalc->currentCal[ch]=DEFAULT_CURRENT_CALIBRATION;
     powerCalc->setNoiseFloor(ch,0.0f); powerCalc->setLpfAlpha(ch,1.0f);`
   - sysData (under `*dataMutex`): `sysData->currentCalibration[ch]=
     DEFAULT_CURRENT_CALIBRATION;`
   - `handled=true`. Place BEFORE the `reset_channel_names|reset_ch_to_default`
     branch (line ~218) so the `"reset_ch_cal"` substring never matches there.
   - Verification: `arduino-cli compile` (command in VERIFICATION.log line 341;
     arduino-cli at `~/apps/arduino-ide/.../arduino-cli`).

2. **Frontend button + sender**
   - Files: `frontend/script.js`
   - In the calibration collapse-body builder (lines ~757-776), add a row:
     ```html
     <div class="cal-param-row">
       <button class="btn-sm btn-danger" onclick="sendResetChannelCal(${idx})">&#8634; Reset Cal</button>
       <span class="hint-inline">(cal, noise floor, LPF)</span>
     </div>
     ```
   - `sendResetChannelCal(idx)`: delete `currCal_${idx}`/`nf_${idx}`/
     `lpf_${idx}` `dataset.userSet` (so next sync repopulates defaults);
     `sendCommand({cmd:'reset_ch_cal', ch:idx}).then(()=> showToast(...))`.
   - Verification: `node --check frontend/script.js`.

## Test Strategy
- Firmware: compile gate only (no native harness); manual WS test on hardware:
  tune ch2 currCal to 150 + LPF 0.5 → `reset_ch_cal ch:1` → values revert to
  100 / 1.0, ch1/ch3 unaffected.
- Frontend: `node --check`; Playwright demo smoke — each channel shows Reset
  Cal button, clicking sends `{"cmd":"reset_ch_cal","ch":N}` and is admin-only
  (inside `admin-only` panel).

## Rollback
- Revert the `reset_ch_cal` branch — old firmware ignores unknown commands.
- Revert the JS button — old dashboard simply won't show it.
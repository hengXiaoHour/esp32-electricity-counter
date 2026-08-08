# Plan: Board Picker Dropdown for Cloud Dashboard

## Goal
Dashboard lists live boards from RTDB `/devices/*` in a dropdown, letting the
user pick which board's live data + console they watch. No config.js editing.
Default selection: the 192.168.100.3 board (`esp-858428`).

## Files to Modify
- `frontend/index.html` — add `<select id="devicePicker">` in the Cloud connect row
- `frontend/style.css` — style the select to match `.connect-row input`
- `frontend/script.js` — populate picker from RTDB `/devices`, track selection,
  use it in `connectCloud()` + `sendCommand()`, rebind on change while in cloud mode

## Implementation Steps
1. **index.html**: add a device `<select>` above the Cloud/Demo buttons with
   id `devicePicker` and a hint "Board to watch in Cloud mode".
2. **style.css**: `.connect-row select` styled like input (mono font, border).
3. **script.js**:
   - Add `let selectedDeviceId = null;` and `devicePickerEl` helper
   - `loadDevicePicker()`: lazy-init firebase app (same as connectCloud),
     `once('value')` on `/devices`, list keys that have a `latest` node,
     fill `<option>esp.xxx</option>`, pre-select default
     (`localStorage esp32monitor_device` → `FB_CONFIG.deviceId` →
     `esp-858428`), store `cloudDb` for reuse.
   - `currentDeviceId()`: returns selected + fallback chain.
   - `cloudDevPath()`: `'devices/' + currentDeviceId()`.
   - `connectCloud()`: use `cloudDevPath()`; show device id in `connectedIp`.
   - `sendCommand()`: use `cloudDevPath()`.
   - on `devicePicker` change: persist to localStorage, and if
     `connMode === 'cloud'` detach old refs + rebind (call `bindCloud()`,
     extracted from `connectCloud`).
   - Persist selection so it survives reloads (replaces editing config.js).
4. Deploy Hosting + verify `?` served page contains dropdown.

## Test Strategy
- Deploy hosting, open https://esp32-electricity-counter.web.app
- Expect both `esp-858428` and `esp-a172e0` options, default `esp-858428`
- Click Cloud → shows data from esp-4011 board; switch dropdown → board data
  flips without reload; commands routed to newly selected board.
- Demo/Local modes unaffected.

## Rollback
- `git checkout frontend/index.html frontend/style.css frontend/script.js`
- Redeploy hosting.
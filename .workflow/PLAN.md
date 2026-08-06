# Plan: v1.2.0 Dashboard Redesign (GRID_ADMIN minimalist, PC + mobile)

## Source of truth
Stitch screens in project `15561552837700085916` (user-designed):
- `minimalist_pc` — dark charcoal, left sidebar (GRID_ADMIN → Dashboard/Analytics/History/Settings), header V/P/I + wifi, channel cards PWR/CUR/ENG + LIMIT bar.
- `minimalist_mobile` — pure black, top V/P/I status bar, stacked channel cards, bottom nav.

## Files to modify (fw repo `data/`)
- `data/index.html` — full restructure: connect panel, `#app` shell (sidebar + status bar + 4 pages + mobile bottom nav), edit modal.
- `data/style.css` — full rewrite: minimalist dark tokens, sidebar layout, mobile bottom-nav breakpoint, channel cards, panels, charts.
- `data/script.js` — full rewrite keeping WS contract + all command/message field names, DOM-preserving updates, adds: page navigation, live canvas charts, OTA progress handling.

## Design decisions (user-approved)
- Implement both PC + mobile as one responsive page (same DOM, CSS switches).
- Desktop: sidebar pages — Dashboard (6 cards), Analytics (live charts), History (event log), Settings (connection + ntfy + calibration + about).
- Mobile: bottom nav — Dashboard / Charts / History / Settings (4 tabs; minor extension of 3-tab mockup for functional completeness).
- Preserve status semantics: ok=green, warning=amber, tripped=red pulsing, off=grey/dim.
- 6 channels (system has 6, mockups show 3).
- Branding: GRID_ADMIN. Sign Out = disconnect. Support = info toast.

## WS contract (must not change)
In: v, ota, otaProgress, voltageCalibration, currentCalibration[6], noiseFloor[6], rmsSamples, firmwareVersion, lastMonth, epoch, ch[6]{n,a,w,kwh,pf,mkwh,s}, events[{t,c,s,m}], ntfy{topic,enabled}.
Out commands: set_voltage_cal, set_current_cal, set_noise_floor, set_rms_samples, set_ntfy_topic, set_ntfy_enabled, set_monthly_kwh, set_name, reset_counter, reset_ch_to_default, reset_nvs_defaults, reset_channel_names.

## Implementation steps
1. `data/index.html` — new structure (IDs preserved: esp32Ip, demoMode, connectStatus, connectPanel, headerVoltage, totalPower, totalCurrent, todayDate, deviceTime, connStatus, fwVersion, otaPanel/otaProgress/otaPercent/otaVersion, channels, eventList, eventCount, ntfyTopic, ntfyEnabled, voltCal, rmsSamples, calibrationRows, resetNvsBtn, editModal + modal* IDs, connectedIp).
2. `data/style.css` — tokens + responsive layout.
3. `data/script.js` — logic + charts + OTA + showPage.
4. Verify: serve data/ locally, run demo mode via headless check (no test framework exists; manual browser check + JS syntax check with node).
5. Commit v1.2.0.

## Test strategy
- `node --check data/script.js` for syntax.
- Serve with python http.server; verify demo mode via quick DOM-less sanity (optional).
- Manual: user opens in browser, Connect → Demo.

## Rollback
- git checkout data/ from pre-existing working tree (already modified — reverted by user-kept HEAD state).

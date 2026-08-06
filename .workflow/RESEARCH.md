# Research: Remove All Relay Features — Pure 6-Channel Counter

## Current State

- ESP32-S3, 6 channels. Relays: GPIO43,44,13,12 (active-LOW) on channels 0-3; channels 4-5 monitoring-only.
- Changeover pair system (`CHANGEOVER_PAIRS`, `PairState`, `manual[]`, `setRelayManual` in `src/core/limit_manager.*`) force-switches relays between counter A/B based on monthly kWh limits.
- `relay_controller.{h,cpp}` — 100% relay-specific GPIO driver. `relayOn` field in `ChannelData` broadcast as `"r"` in WS JSON.
- NVS keys `relay_1..4`, `pair_man_1..2` persist relay state + manual override.
- WS commands `set_relay` (relay control) + `reset_relay` (actually = counter reset! must be kept/renamed).
- UI `data/script.js` is pair-centric: pair cards, `toggleRelayPair`, MANUAL badges, `optimisticRelays`, `CHANGEOVER_PAIRS`, `manual[]`, `r`.
- `power_calculator`, `current_sensor`, `voltage_sensor`, `wifi_manager`, `ota_handler`, `ntfy_notifier`, `status_led.cpp` — ZERO relay references (verified clean).

## Requirements (user-confirmed)

- **Remove ALL relay features completely.** No relays, no changeover pairs, no manual override, no relay control.
- Firmware becomes a pure measurement device: 6 counters reading sensor input.
- Dashboard shows 6 independent channel cards (no pair cards / relay buttons).

## Non-relay behavior that must be PRESERVED

- Monthly rollover (`rolloverIfNeeded`): NTP-synced month change → zero all 6 energies, `saveLastMonth`, statuses → OK, log "Monthly reset" event. Remove only the relay re-energize loop.
- `resetCounter`: zero energy + NVS persist + status OK. Remove only the pair-flip block.
- `logEvent` ring buffer + WS event broadcast + dashboard toast.
- WS command `reset_relay` → RENAME to `reset_counter` (it is counter reset, not relay control).
- `test_force_rollover` WS hook (rollover test tool).
- Calibration panel (set_voltage_cal / set_current_cal / set_noise_floor / set_rms_samples), `test_inject`, `set_monthly_kwh`, ntfy topic/enabled settings, channel names.
- `ntfy_notifier` class (relay-independent HTTPS push).

## GAP / Open Question

The WARNING/TRIPPED status machine, LED blink, and ntfy push are **entirely embedded in the changeover-pair machine** — there is NO standalone per-channel limit check today (`WARNING_THRESHOLD_PCT` is dead code, channels 5-6 never warn/trip). Removing relays without a replacement silently kills status/LED/ntfy for all channels.

## Risks

1. `ChannelData::relayOn` + WS `"r"` field + `manual[]` + `hasRelay` must be removed together, or the UI silently shows all counters "B / inactive".
2. `reset_relay` is actually counter-reset — deleting it as "relay code" breaks the Reset Counter button.
3. GPIO12 is a strapping pin (MTDI) — currently driven by relay_controller. If the relay board stays wired, leaving it floating can change boot strapping. Confirm hardware detached or add explicit pull-down.
4. Orphaned NVS keys `relay_*`/`pair_man_*` are harmless (unread); do NOT use `clearAll()` (would nuke calibrations). Optionally `prefs.remove()` in begin().
5. Tests: `tests/test_cal_sync.py` is relay-free but drives the calibration UI — must still pass after the dashboard rewrite (depends on `#demoMode`, `calibrationRows`, `currCal_#`, `nf_#`, `rmsSamples`, `voltCal`, `resetNvsBtn` surviving).
6. Docs: README.md, ARCHITECTURE.md, AGENTS.md reference relays — update after code.
7. Last session's in-flight manual-relay feature (uncommitted) is being superseded, not extended.

## Approach Options

- **A (recommended):** Strip to pure measurement + KEEP per-channel limit warnings. Rework `limit_manager` into relay-free manager: per-channel check `energyKWh >= monthlyKwhLimit` → WARNING/TRIPPED status + ntfy + LED + event. Preserves all existing monitoring UX without relays.
- **B:** Strip to pure measurement only. `limit_manager.loop()` = `rolloverIfNeeded()` alone. No status changes, no ntfy, no LED limit indication. Simplest, smallest flash.
- **C:** Hybrid — pure measurement, but keep ntfy + monthly rollover + resetCounter + event log; no per-channel limit trip.

## Files Affected

- DELETE: `src/core/relay_controller.{h,cpp}`
- MODIFY: `src/config.h`, `src/core/limit_manager.{h,cpp}`, `src/network/websocket_server.{h,cpp}`, `src/utils/nvs_manager.{h,cpp}`, `esp32-electricity-counter.ino`
- REWRITE: `data/script.js` (6 independent cards), `data/style.css`, `data/index.html`
- UPDATE: `README.md`, `doc/ARCHITECTURE.md`, `doc/opencode_agent/AGENTS.md`, `src/ui/status_led.h` (comment)

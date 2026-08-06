# Research: Migrate Dashboard to Firebase (Option B — RTDB Bridge)

## Current State

- **Firmware**: ESP32-S3, Arduino C++. `networkTask` (Core 0) broadcasts a full JSON snapshot
  every 150 ms via `WebSocketServer::broadcastData()` when ≥1 WS client connected.
- **Dashboard**: `frontend/` (index.html, style.css, script.js, manifest.json, sw.js, icons/).
  Browser connects `ws://<ip>/ws` (script.js:86), receives flat snapshot, renders via the
  single `updateDashboard(data)` function. Sends `{cmd: ...}` commands.
- **No Firebase anywhere** in the project. Hosting model per AGENTS.md: UI hosted externally,
  ESP32 only runs the WS server.
- **PWA scaffolding already present** (manifest, sw.js, icons) but inert over plain `http://`
  LAN because service workers need a secure context.

## Requirement

Host the dashboard publicly + make it a real PWA. User chose **Option B**: Firebase as the
bridge. ESP32 connects **outbound** to Firebase RTDB (no port forwarding), browser subscribes
via Firebase JS SDK. This makes the dashboard work from anywhere AND gives HTTPS (PWA activates).

## Protocol Contract (must be preserved)

### Snapshot (ESP32 → dashboard) — flat object, all fields:
`v, uptime, wifi, rssi, ap, ota, otaProgress, voltageCalibration, currentCalibration[6],
rmsSamples, noiseFloor[6], firmwareVersion, epoch, lastMonth, ntfy{topic,enabled},
ch[6]={n,a,w,va,pf,kwh,s,mkwh}, events[last10]={t,c,s,v,m}`

### Commands (dashboard → ESP32):
`set_name{ch,name}`, `reset_counter{ch}`, `test_inject{ch,val}`, `set_voltage_cal{val}`,
`set_current_cal{ch,val}`, `set_monthly_kwh{ch,val}`, `set_noise_floor{ch}`, `set_rms_samples{val}`,
`set_ntfy_topic{val}`, `set_ntfy_enabled{val}`, `reset_ch_to_default{ch}`, `reset_nvs_defaults`,
`test_force_rollover`

### Firmware internals:
- `SystemData` (config.h:127), guarded by `dataMutex`. Populated by `updateSharedData()`.
- `buildJson()` (websocket_server.cpp:282) assembles the snapshot; reads firmwareVersion/epoch/
  lastMonth/ntfy/noiseFloor live.
- `handleCommand()` (websocket_server.cpp:88) parses commands via naive `indexOf` matching.
- NVS namespace `"elec-counter"`. No Firebase libs.

## Approach Options

1. **RTDB with Firebase-ESP-Client lib** (mobizt) — ESP32 uses service-account JWT auth,
   RTDB streaming for commands. Robust, standard. Adds a library dependency.
2. **RTDB via raw HTTPS REST + legacy DB secret** — no big lib, but secrets are deprecated
   and rules security is coarser.
3. **Firestore** — more structure but overkill; ESP32 REST + realtime listeners harder.

**Recommendation: Option 1 (RTDB + Firebase-ESP-Client)** with the Dashboard keeping its
`updateDashboard(data)` rendering. Data goes into a single `latest/` node (browser `.on('value')`),
commands go into a `commands/` push queue (ESP32 polls + deletes).

## Key Design Decisions

1. **Update cadence**: 150 ms WS is too hot for RTDB. Use ~1 s for the full snapshot (Spark
   tier: 100 MB/month free bandwidth — plenty for a 6-ch monitor at 1 s).
2. **Arrays in RTDB**: RTDB stores JS arrays as numeric-keyed objects. Dashboard watcher must
   normalize `{0:..,1:..}` → `[...]` for `ch`, `currentCalibration`, `noiseFloor`, `events`
   before calling `updateDashboard`.
3. **Command queue**: dashboard does `commands.push({cmd,ch,val,name})`; ESP32 reads children,
   executes, deletes. Fire-and-forget matches current semantics.
4. **Security rules**: `latest/` public read (device writes via auth), `commands/` public write,
   `.write` only from service-account on `latest/`. Enforce via rules.
5. **Dashboard connection**: replace IP connect panel with direct Firebase bootstrap; keep demo
   mode as fallback.
6. **Hosting**: `firebase.json` in repo root, `public: frontend/`. PWA (manifest/sw.js) then works
   because hosting is HTTPS.

## Risks

- Firebase-ESP-Client is a big lib (~big RAM/flash); must verify it compiles in the existing
  dual-core task layout and fits flash.
- RTDB write latency/failures need retry + last-writer-wins; ESP32 must keep `latest/` fresh
  even when dashboard is closed (it already broadcasts unconditionally).
- NTP/time and `epoch` still needed; unchanged.
- OTA still via Arduino IDE network port — dashboard only *displays* progress (unchanged).
- Rules mistakes → open database. Must set rules on deploy.

## Open Questions

- Create RTDB instance at which location? (default us-central1 fine)
- Keep the old WS path as fallback alongside Firebase, or fully replace? (Recommend full replace
  in the dashboard; keep WebSocketServer in firmware only if user wants LAN fallback.)

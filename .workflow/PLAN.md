# Plan: Firebase RTDB Bridge + Cloud/Local Dashboard (Option B)

## Decisions (user-confirmed)
- **Keep both**: dashboard lets user choose **Cloud** (Firebase) or **Local** (ws://<ip>) in the connect panel.
- **Auth**: Firebase-ESP-Client library (mobizt) + service-account JWT on the ESP32.
- **Cadence**: full snapshot pushed to RTDB every **1 s** (WS LAN path stays at 150 ms).

## Architecture
```
Firebase RTDB (esp32-electricity-counter)
├── latest/            device writes full snapshot every 1s (browser reads)
└── commands/          browser pushes {cmd,ch,val,name,ts}; device polls+deletes
```

Browser: `ref('latest').on('value')` → normalize `{0:..}` arrays → `updateDashboard(data)`.
Commands: `commands.push({cmd,...})`. Device: list children → execute → remove.

## Files to Create
- `src/network/firebase_bridge.h` — `FirebaseBridge` class (start, update, poll commands)
- `src/network/firebase_bridge.cpp` — impl (Firebase-ESP-Client RTDB)
- `src/network/firebase_config.h` — **gitignored**: `FIREBASE_API_KEY`, `FIREBASE_DB_URL`, `FIREBASE_SA_EMAIL`, `FIREBASE_SA_KEY` (PEM)
- `src/network/firebase_config.example.h` — committed template with placeholders
- `firebase.json` — hosting config (public: `frontend/`, rewrites)
- `database.rules.json` — RTDB security rules
- `frontend/config.example.js` — dashboard Firebase web-app config template
- `frontend/config.js` — **gitignored** dashboard Firebase config (apiKey, dbURL, appId)

## Files to Modify
- `esp32-electricity-counter.ino` — start FirebaseBridge in networkTask; call update + command poll
- `src/network/websocket_server.cpp` — extract `handleCommand()` body into a shared `processCommand(json)` so both WS + Firebase use it
- `src/network/websocket_server.h` — expose shared command handler
- `frontend/index.html` — connect panel: Cloud/Local tabs/choice; load config.js + Firebase SDK
- `frontend/script.js` — Cloud mode (Firebase connect, latest listener, array normalization, commands.push); keep Local WS + demo modes
- `frontend/sw.js` — add Firebase SDK files to cache shell
- `.gitignore` — add `firebase_config.h`, `frontend/config.js`, `.firebase/`
- `README.md` — Firebase setup + deploy instructions (doc-sync later)

## Implementation Steps
1. **Scaffold Firebase hosting + RTDB** (CLI)
   - `firebase init hosting` → public `frontend/`, SPA rewrite
   - `firebase init database` → create `database.rules.json`
   - Verify: `firebase deploy --only hosting` serves index.html over HTTPS
2. **Firmware — Firebase-ESP-Client install + compile**
   - `arduino-cli lib install "Firebase-ESP-Client"`
   - Add `firebase_config.h` (example + real), `firebase_bridge.{h,cpp}`
   - Refactor `handleCommand` → shared `processCommand()`
   - networkTask: `fb.begin()`; every 1s `fb.setJSON(latest)`; every 500ms poll+execute `commands/`
   - Verify: `arduino-cli compile` PASS; flash ≤ 1310720
   - **Risk gate**: if lib pushes flash >100%, fall back to minimal REST+secret approach (documented)
3. **Dashboard — Cloud mode in JS**
   - Connect panel: "Cloud (Firebase)" / "Local (ESP32 IP)" / "Demo" choice
   - `config.js` initFirebase; `latest` listener → normalize arrays → `updateDashboard`
   - `sendCommand()` → `commands.push(...)` (reuse all existing send* functions)
   - Keep WS + demo paths unchanged
   - Verify: `node --check frontend/script.js`; headless Chrome demo harness with a fake RTDB
4. **Rules + deploy**
   - `database.rules.json`: `latest` read public / write `auth!=null`; `commands` read+write public
   - `firebase deploy` (hosting + database)
   - Verify: install PWA on phone/desktop from https URL (Lighthouse-style checks via headless Chrome)

## Test Strategy
- `tests/test_cal_sync.py` regression (existing)
- Firmware: compile gate (flash/RAM budget)
- JS: `node --check`, headless Chrome harness with Firebase emulator or mocked RTDB
- Firebase emulator (local) for dashboard dev before real deploy

## Rollback
- `git revert` firmware + dashboard changes; Firebase project can be deleted via CLI/console.
- WS path remains intact throughout — dashboard never loses LAN functionality.

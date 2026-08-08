# Plan: Web UI Live Console Terminal

## Goal
Add a terminal-style console to the Settings tab that works over both Local (WebSocket) and Cloud (Firebase) connections, giving web UI users the same diagnostic power as the serial port.

## Scope
- **Included**: All read-only commands (`status`, `ch`, `cal`, `info`, `wifi`, `help`) + safe writes (`buzz`, `inject`, `reset`, `reset_name`)
- **Excluded (local-only)**: `test led`, `nvs_debug`, `reboot`, `setwifi connect/save`, `clearwifi` — too destructive or blocking for remote use

---

## Files to Create

| File | Purpose |
|---|---|
| `src/network/console_handler.h` | `ConsoleHandler` class — executes text commands, fills response buffer |
| `src/network/console_handler.cpp` | Implementation of all console commands (extracted from .ino serial handler) |

## Files to Modify

| File | Changes |
|---|---|
| `src/network/command_processor.cpp` | Add `console` command case; add optional `String *responseOut` param to `processCommand()` |
| `src/network/command_processor.h` | Update signature |
| `src/network/websocket_server.cpp` | Send console response via `client->text()` instead of discarding |
| `src/network/firebase_bridge.cpp` | Write console response to `/console/` node after executing command |
| `src/network/firebase_bridge.h` | Minor updates if needed |
| `esp32-electricity-counter.ino` | Wire `ConsoleHandler` into setup; remove serial command handler (keep serial echo/prompt only) |
| `database.rules.json` | Add `/console` node rules |
| `frontend/index.html` | Add "Device Console" panel to Settings page |
| `frontend/style.css` | Add terminal styles |
| `frontend/script.js` | Console UI logic (send/receive/display) |

---

## Implementation Steps

### Step 1 — ConsoleHandler class
- Extract command logic from `handleSerialCommand()` into `ConsoleHandler`
- Method: `void exec(const String &line, String &out)`
- Safe subset only: `status`, `ch`, `cal`, `info`, `wifi`, `help`, `buzz`, `inject`, `reset`, `reset_name`
- Output appended to `String &out` with `\n` separators
- Unknown → `"Unknown command. Type 'help'.\n"`
- Need access to: `nvs`, `powerCalc`, `systemData`, `dataMutex`, `buzzer`, `limitMgr`

### Step 2 — processCommand() response support
- Add `String *responseOut = nullptr` parameter
- Add `console` case: extract `"line"` field, call `consoleHandler->exec(line, *responseOut)`
- Existing call sites remain compatible (nullptr = no capture)

### Step 3 — WebSocket response path
- In `handleCommand()` (websocket_server.cpp), stop discarding `client`
- After `processCommand()`, if responseOut non-empty → `client->text(jsonEncodedResponse)`
- JSON format: `{"type":"console","out":"escaped text here"}`
- Frontend `ws.onmessage` checks `data.type === 'console'` → append to terminal

### Step 4 — Firebase response path
- In `pollCommands()`, after `processCommand()` returns handled with response
- Write response to `/console/<key>` via `Firebase.RTDB.setString()`
- Delete the original command node
- Add rule: `console: { ".read": true, ".write": "auth != null" }`
- Frontend subscribes `cloudDb.ref('console').on('child_added')` → append to terminal, then delete node

### Step 5 — Web UI console panel
- New `<div class="panel-box" id="consolePanel">` in `#page-settings`
- Output area: `<div id="consoleOutput" class="console-output"></div>`
- Input row: `<input id="consoleInput" class="console-input" placeholder="Type command...">` + Send button
- Enter key or button click → `sendCommand({cmd:'console', line: input.value})`
- Display incoming `type:'console'` messages in output area
- Auto-scroll to bottom

### Step 6 — Wire + verify
- Update `.ino`: instantiate `ConsoleHandler`, pass to modules that need it
- Compile gate: arduino-cli, flash/RAM budget check
- Runtime test: send `help`, `status`, `buzz 3` via web UI
- Cloud test: same via Firebase deploy

---

## JSON Escaping

Need a helper to escape `"` → `\"`, `\n` → `\\n`, `\` → `\\` for embedding response text in JSON. Add to `console_handler.h` as static utility.

---

## Test Strategy
- Compile gate (arduino-cli, flash budget)
- `node --check script.js`
- Manual: open web UI (local), type `help`, `status`, `ch 1`, `buzz 2` → verify output
- Manual: open web UI (cloud), same commands → verify output
- Edge: unknown command, empty input, very long output (truncation)

---

## Rollback
- Revert firmware: `git revert` — serial handler returns, web console panel inert
- Revert frontend: panel hidden, commands ignored
- Firebase: `/console` node can be deleted, rules reverted

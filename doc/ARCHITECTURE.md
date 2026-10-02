# Architecture — ESP32-S3 6-Channel AC Electricity Counter

This file is the canonical reference for the project: **what the structure is**
(Part A, locked) and **how it actually works** (Part B, derived from the code).

Every claim in Part B was read out of the current source. Where the code and the
older prose docs disagree, Part B wins and the discrepancy is recorded in
[Part C — Documentation drift](#part-c-documentation-drift).

---

# Part A — Project Structure (LOCKED)

## Architecture Lock

This structure is **LOCKED**. All AI agents MUST read this file before creating
or modifying any files, and MUST NOT create files or directories outside this
layout without explicit user approval.

Machine-readable lock: `./.architecture.lock.json`

Allowed directories (from the lock file): `doc`, `src`, `src/sensor`, `src/core`,
`src/network`, `src/ui`, `src/utils`, `frontend`, `scripts`, `.workflow`,
`doc/opencode_agent`.

## Directory Tree

```
esp32-electricity-counter/
├── esp32-electricity-counter.ino    # Entry point: setup() + loop() + task creation
├── opencode.json                    # Agent config
├── .gitignore
├── README.md                        # Setup instructions
├── design.md                        # Dashboard visual style + design decisions
├── firebase.json                    # Hosting (public=frontend/, SPA rewrite) + RTDB rules ref
├── .firebaserc                      # Firebase project alias
├── database.rules.json              # RTDB security rules (deployed)
│
├── doc/                             # Documentation
│   ├── ARCHITECTURE.md              # THIS FILE
│   ├── .architecture.lock.json      # Machine-readable lock (do not edit)
│   ├── esp32s3-electricity-counter-prompt.md  # Original build prompt
│   └── opencode_agent/              # Agent context (auto-managed)
│       ├── AGENTS.md
│       ├── lessons.md
│       └── memories.json
│
├── src/
│   ├── config.h                     # Pins, #define constants, shared structs/enums
│   ├── core/
│   │   ├── power_calculator.h/.cpp  # ALL ADC sampling + RMS + P/PF/kWh math
│   │   └── limit_manager.h/.cpp     # Budget state machine, buzzer, events, rollover
│   ├── network/
│   │   ├── wifi_manager.h/.cpp      # STA connect + AP fallback + reboot failsafe
│   │   ├── ap_portal.h              # PROGMEM captive-portal page (HTML/CSS/JS inline)
│   │   ├── websocket_server.h/.cpp  # AsyncWebSocket + shared :80 server
│   │   ├── firebase_bridge.h/.cpp   # RTDB /latest push + /commands poll + buildSystemJson
│   │   ├── firebase_config.h        # REAL credentials (gitignored, placeholders now)
│   │   ├── firebase_config.example.h
│   │   ├── command_processor.h/.cpp # JSON command parser shared by WS + Firebase
│   │   ├── console_handler.h/.cpp   # Text console engine shared by serial + web
│   │   ├── ntfy_notifier.h/.cpp     # ntfy.sh push (queued on Core 1, sent on Core 0)
│   │   └── ota_handler.h/.cpp       # ArduinoOTA wrapper
│   ├── ui/
│   │   ├── status_led.h/.cpp        # WS2812 (with R/G swap workaround)
│   │   └── buzzer.h/.cpp            # Non-blocking beep pattern driver
│   ├── utils/
│   │   ├── nvs_manager.h/.cpp       # Preferences wrapper (all persistence)
│   │   ├── device_id.h/.cpp         # "esp-<hex>" id from efuse MAC
│   │   └── log_gate.h               # STATUS_LOG / DEBUG_LOG serial stream gates
│   └── sensor/
│       └── current_sensor.h/.cpp,   # ⚠ DEAD CODE — not included by anything;
│           voltage_sensor.h/.cpp    #   superseded by PowerCalculator (see Part C.1)
│
├── frontend/                        # Dashboard PWA — hosted on Firebase Hosting
│   ├── index.html
│   ├── style.css
│   ├── script.js                    # WS + RTDB client, 3 modes, all UI logic
│   ├── config.js                    # Firebase web config (gitignored)
│   ├── config.example.js
│   ├── manifest.json
│   ├── sw.js                        # Service worker (network-first + shell fallback)
│   └── icons/
│
├── scripts/
│   ├── setup.py
│   └── deploy.py
│
├── tools/
│   ├── firebase_rest.py             # RTDB admin REST helper
│   └── hosting_deploy.py
│
└── .workflow/                       # Agent workflow state
    ├── active.json
    ├── RESEARCH.md
    ├── PLAN.md
    └── VERIFICATION.log
```

## Conventions

| Aspect | Rule |
|---|---|
| **Arduino sketch** | `.ino` at project root, named `esp32-electricity-counter.ino` |
| **C++ sources** | `snake_case.h` / `snake_case.cpp` in `src/` |
| **Header guards** | `#pragma once` |
| **Documentation** | `doc/` directory only |
| **Web files** | `frontend/` directory |
| **Naming** | `snake_case` for files, `PascalCase` for classes |
| **Indentation** | 2 spaces |
| **Platform** | Arduino IDE / Arduino framework (not ESP-IDF, not PlatformIO) |
| **Target** | ESP32-S3 |
| **Board** | `ESP32S3 Dev Module`, `No FS 4MB (2MB APP with OTA)`, PSRAM on |

## Lock Enforcement

1. **Before creating any file**, read this file and `.architecture.lock.json`
2. If the new file path fits the tree above — proceed
3. If a directory doesn't exist yet but fits logically — ask the user
4. If the file doesn't fit the structure at all — BLOCKED, explain why

---

# Part B — How It Works

## B.1 System Shape

Three cooperating pieces:

```
   ┌──────────────────────┐        ┌────────────────────────────┐
   │  ESP32-S3 firmware   │        │  Firebase                  │
   │                      │        │                            │
   │  Core 1: sensing     │  push  │  RTDB                      │
   │  Core 0: transport   │───────▶│   /devices/<id>/latest    │
   │                      │  poll  │   /devices/<id>/commands   │
   │  AsyncWebSocket :80  │◀──────▶│   /devices/<id>/console    │
   │   /ws  + AP portal   │        │   /devices/<id>/viewers    │
   └──────────┬───────────┘        │  Hosting (static)          │
              │                    │   frontend/  ── PWA        │
              │ LAN                └──────────┬─────────────────┘
              │                               │ HTTPS
   ┌──────────▼───────────┐        ┌──────────▼─────────────────┐
   │  Browser: Local mode │        │  Browser: Cloud mode       │
   │  ws://<ip>/ws        │        │  firebase SDK (Google auth)│
   └──────────────────────┘        └────────────────────────────┘
```

The device never serves the dashboard. It only serves a WebSocket endpoint (plus
a tiny provisioning page while in AP mode). The dashboard is a static PWA on
Firebase Hosting.

**Data direction is one-way for telemetry** (device → cloud, fire-and-forget
overwrite of a single node) and **command direction is one-way too** (dashboard
pushes a node, device polls and deletes it). There is no bidirectional socket to
the cloud — this keeps the firmware simple and survives the Firebase client
library's blocking TLS model.

## B.2 Hardware & Signal Chain

| Signal | Pins | Device |
|---|---|---|
| CT current ch1–ch6 | GPIO7, 5, 6, 8, 4, 2 | SCT-013-100 |
| Voltage reference | GPIO1 | ZMPT101B |
| Active buzzer | GPIO13 | — |
| RGB LED | GPIO48 | WS2812 (R/G physically swapped → compensated in software) |

ADC is 12-bit (`analogReadResolution(12)`), 0–4095 counts over a 3.3 V reference.
Both CT and ZMPT outputs are **AC-coupled and biased to mid-supply**
(`AC_BIAS_VOLTAGE 1.65f`), so every RMS computation is mean-removed.

Bluetooth is shut down at boot (`btStop()` + `esp_bt_controller_mem_release`)
to reclaim RAM and idle current — the board never uses BT.

## B.3 Runtime: Three Tasks, Two Cores

`setup()` creates the tasks; `loop()` stays nearly empty on purpose.

| Task | Core | Prio | Stack | Period | Responsibility |
|---|---|---|---|---|---|
| `networkTask` | 0 | 2 | 8192 | 20 ms | WiFi, WebSocket broadcast, serial RX, OTA, ntfy TX, eco decision |
| `sensorTask` | 1 | 2 | 8192 | 80 ms | ADC sampling, power math, limit check, buzzer, LED, NVS energy save |
| `firebaseTask` | 0 | **1** | 32768 | 50 ms | `Firebase.begin()`, `/latest` push, `/commands` poll, `/viewers` poll |

Plus Arduino's default `loopTask` on Core 1, which only drives
`consoleHandler.runDeferred()` / `consoleHandler.loop()`.

**Why the priority split matters.** `networkTask` is priority 2 and
`firebaseTask` is priority 1 *on the same core*. Firebase's HTTPS/TLS calls are
blocking; if they ran at equal priority they would delay the 150 ms WebSocket
broadcast. Lowering `firebaseTask` guarantees LAN responsiveness no matter how
slow a cloud push is.

**Why `Firebase.begin()` is not in `networkTask`.** The initial TLS handshake
plus service-account token exchange takes 10–20 s. Doing that inside
`networkTask` starves IDLE0 and trips the watchdog. It is deferred to
`firebaseTask`, which only starts it once WiFi is up.

**Why `ArduinoOTA.begin()` is deferred too.** Started before the interface has
an IP, it silently never listens (most visible in AP mode). So `startServer()`
and `otaHandler.begin()` are both triggered from `networkTask` after WiFi is up
or the AP is running.

## B.4 Shared State & Mutex Discipline

One global `SystemData` struct is shared across cores, guarded by
`dataMutex` (a FreeRTOS mutex, never a binary semaphore).

`SystemData` (`src/config.h`) holds:
- `channels[6]` — name, currentRMS, activePower, apparentPower, powerFactor,
  energyKWh, monthlyKwhLimit, status
- voltageRMS, voltageCalibration, `currentCalibration[6]`, rmsSamples, uptime
- wifiConnected, wifiRSSI, apMode
- `events[50]` ring + eventCount
- otaInProgress, otaProgress

Rules the code actually follows:

- **Writers hold the mutex.** `updateSharedData()`, `limitMgr.loop()`,
  `buildSystemJson()`, and every mutating command take it.
- **Readers take it too** before serialising — `networkTask` takes it around
  `broadcastData()`, `firebaseTask` around `buildSystemJson()`.
- Timeouts are bounded (20–100 ms) and the result is **checked**; on timeout the
  cycle is skipped rather than blocking.
- `PowerCalculator`'s internal sample/derived arrays are **not** mutex-protected.
  They are only touched by `sensorTask` (and by auto-zero, which also runs on
  `sensorTask`), so they are effectively core-local. The mutex protects the
  *published* copy in `SystemData`, not the calculator.

## B.5 Measurement Path (Core 1)

Everything happens inside `PowerCalculator`, called once per 80 ms sensor cycle
from `sensorTask`. `src/sensor/*` is not involved (see Part C.1).

### 1. Interleaved sampling — `collectSamples()`

```
for i in 0 .. rmsSamples-1:
    voltageSamples[i] = analogRead(GPIO1)
    for ch in 0..5:  currentSamples[ch][i] = analogRead(CT_PIN[ch])
    delayMicroseconds(40)          // ~25 kHz
```

Voltage is read first each iteration, then all six currents — so any per-channel
skew is identical, which is what makes the cross-product meaningful.
`rmsSamples` is runtime-tunable (100…2000, default 1000 = `MAX_RMS_SAMPLES/2`);
`SAMPLES_PER_CYCLE` (500) documents the ~500 samples per 50 Hz cycle target but
is not referenced by code (see Part C.4).

### 2. Voltage RMS

Mean-removed variance → ADC RMS → pin volts → calibration multiplier:

```
vMean     = mean(voltageSamples)
vAdcRMS   = sqrt( mean( (v - vMean)^2 ) )
vPinV     = vAdcRMS / 4095 * 3.3
voltageRMS = vPinV * voltageCal
```

### 3. Current RMS, noise floor, LPF — per channel

Variance is computed as `E[x²] − mean²` (equivalent to mean-removed, one pass):

```
iAdcRMS  = sqrt( E[i²] - mean(i)² )
rawRMS   = iAdcRMS / 4095 * 3.3 * currentCal[ch]
```

Then the **noise floor is subtracted in the power domain**, which is the right
place for it (RMS is a magnitude, so you cannot subtract linearly):

```
signalRMS = sqrt( rawRMS² - floor² )      // 0 if rawRMS <= floor
```

`signalRMS` is then smoothed by a per-channel EMA:

```
filtered += alpha * (signalRMS - filtered)   // alpha = lpfAlpha[ch], default 0.2
```

The first sample after any alpha/noise-floor change initialises `filtered`
directly (via the `rmsInit` flag) instead of ramping up from zero.

### 4. Real power — the cross-product

```
pMean = mean( (v - vMean) * i_raw )          // per sample, per channel
pWatts = pMean * (3.3/4095)^2 * voltageCal * currentCal[ch]
activePower = |pWatts| - voltageRMS * floor   // noise power removed
activePower = max(activePower, 0)
```

Note `i_raw` is the *un*-mean-removed current sample. That is correct rather than
sloppy: `(v − vMean)` is zero-mean by construction, so the current's DC bias
contributes `iMean × mean(v − vMean) = 0` and cancels out of the mean. Taking
`|pWatts|` handles the case where the CT is oriented backwards.

The `voltageRMS * floor` subtraction removes the power attributable to the
measured noise floor, then clamps at zero so a noisy channel cannot report
negative watts.

### 5. Apparent power, PF, energy

```
apparentPower = vActual * filteredCurrentRMS
powerFactor   = (apparentPower > 0.001) ? clamp(activePower / apparentPower, 0, 1) : 0
energyKWh    += activePower * (deltaSeconds / 3600) / 1000
```

Energy integrates **every** sensor cycle using the real elapsed time measured
with `millis()`, so it is independent of the nominal 80 ms period.

### 6. Auto-zero (noise-floor capture)

Triggered by `set_noise_floor` with no `val` (dashboard) or `auto_zero <ch>`
(console). It is **queued per channel** so all six can be requested at once
without the sensing loop stalling.

- `requestAutoZero(ch)` appends to a FIFO; duplicates are rejected.
- `AZ_BATCHES = 32` captures of a full `collectSamples()+computeAll()` pass.
- The sensor loop does **2 batches per cycle** (`AZ_BATCHES_PER_CYCLE`), so a
  32-batch capture spreads over ~16 cycles ≈ 1.3 s of normal operation.
- While capturing, the target channel's noise floor is forced to 0 and its LPF is
  bypassed (`azLpfForced` → alpha 1.0) so the raw floor is measured, not a
  filtered estimate. **The stored `lpfAlpha[ch]` is never overwritten**, so the
  user's setting survives.
- `autoZeroFinish()` sorts the 32 samples and takes the **median** (robust against
  outliers, unlike a mean), commits it via `setNoiseFloor()` + NVS, and resets
  `rmsInit` for all channels.
- Progress (`azActive`, `azChannel`, `azProgress`, `azQueue`) is published in
  every broadcast so the dashboard can show "calibrating Ch1 (18/32) · waiting:
  Ch3, Ch5" and auto-advance.

## B.6 Budget, Trip & Alert State Machine

`LimitManager::loop()` runs every sensor cycle, under the mutex, and does four
things: `rolloverIfNeeded()`, `checkLimits()`, `updateBuzzer()`, and a one-shot
boot event.

### Trip

Per channel, when `limit > 0 && energy >= limit`:

- **First crossing only** (`tripNotified[ch]` latch): append a forensic event,
  fire one ntfy push. This is the anti-flood latch.
- If power factor drops below `AUTO_RECOVER_PF` (0.1) — i.e. the load was removed
  — status returns to `STATUS_OK` and one `autoRecoverLogged` event is written.
  **The trip latch stays set on purpose**: energy is still over budget, so
  clearing it would re-trip and re-notify on the very next 80 ms cycle. The
  status shows "recovered" while the alarm is still latched.
- Otherwise status is `STATUS_TRIPPED`.

Re-arming only happens in the `else` branch — once energy is genuinely back under
the limit (manual reset or monthly rollover).

### Buzzer

While **any** channel is tripped, `updateBuzzer()` keeps beeping forever,
round-robining through the tripped channels: ch1 → 1 beep, ch2 → 2 beeps, …
ch6 → 6 beeps (120 ms on / 140 ms off). It re-arms only when the buzzer is idle,
so a pattern is never cut short. `resetCounter()` and rollover call
`buzzer->stop()`.

### LED

`updateLED()` is a strict three-way priority — and notably **trip state is not
one of them**:

| Condition | Mode |
|---|---|
| OTA in progress | solid blue |
| WiFi not connected | solid red |
| otherwise | solid green |

`LED_BLINK_RED` / `LED_BLINK_YELLOW` exist but are only reachable from the
`test led` console command; `status_led.h` documents this explicitly. The
README's "RED LED blink on trip" is therefore stale (Part C.2).

### Monthly rollover

Anchored to `MONTHLY_RESET_DAY = 25` (so the billing period is the 25th → 24th):

```
if (day >= 25) month += 1            // with year wrap
billingMonth = YYYYMM
if (billingMonth != nvs->loadLastMonth()) → zero all counters, clear latches,
                                          stop buzzer, persist month immediately
```

It waits for a valid clock (`now > 1600000000`, i.e. NTP synced) and is
re-evaluated every sensor cycle. There is deliberately **no once-per-boot
latch**: the persisted billing month is the idempotency guard, so once a
rollover has committed, later cycles short-circuit at the month comparison.
A latch here is actively harmful — on a fresh board the stored month is `0`, so
the first valid NTP would consume the single allowed rollover and the real month
boundary would then be skipped for the rest of that boot session.

The zeros and the new billing month are written in **one** `commit()`, so a
reboot can never restore stale kWh alongside an already-advanced month.

### Forensic event persistence

The RAM ring holds 50 events and is wiped by a reboot — and Firebase is
unreachable while WiFi is down, which is exactly when a trip happens. So the
last `FORENSIC_KEEP = 10` events are also written to NVS on every *critical*
event (trip, manual reset, rollover, energy inject, boot). That is a few writes
per day, negligible for flash wear. `setup()` restores them into `SystemData`
before the first Firebase push, so the trail rides the normal reconnect.

## B.7 Persistence (NVS)

`NVSManager` wraps Arduino `Preferences` and is the only writer to flash.
**`commit()` is required** for anything to survive reboot, and the code is
disciplined about it: every mutating command ends with `nvs->commit()`, rollover
commits immediately, and `flushEnergyToNvs()` runs as a pre-restart hook.

| Group | Keys |
|---|---|
| WiFi | ssid, pass, mode (0=AUTO, 1=STA only, 2=AP only) |
| Per channel | name, monthly kWh limit, current cal, noise floor, LPF alpha, energy kWh |
| Global | voltage cal, rms samples, ntfy topic/enabled, last billing month, forensic events |

**Energy durability** is a two-tier scheme, because losing months of kWh to a
power cut is unacceptable:

1. `sensorTask` writes all six counters **and commits** every ~5 s. The commit is
   essential — `saveEnergyKWh` only stages a `putFloat` in the `Preferences`
   handle, so without it the values never reach flash and any restart silently
   reverts the counters to whatever the last command happened to commit.
2. `flushEnergyToNvs()` is registered as `WiFiManager`'s pre-restart hook, so the
   STA connect-timeout reboot path also flushes — the 5 s save alone can lag a
   reboot by a full interval. It takes `dataMutex` even though it runs from
   Core 0 with no lock held, because `commit()` is `prefs.end(); prefs.begin()`
   and is not thread-safe against the Core 1 save.

## B.8 Connectivity & Failover

`WiFiManager` is a small state machine: `WIFI_INIT → WIFI_CONNECTING →
WIFI_CONNECTED | WIFI_AP_MODE`.

- **STA**: `configTime(0, 0, pool.ntp.org, time.google.com)` on every connect
  attempt (needed for billing-month math). Tx power 21 dBm, modem sleep off.
- **Retry ladder**: reconnect every 10 s, up to `WIFI_MAX_RETRIES = 5`, then fall
  back to AP mode.
- **Reboot failsafe**: if no link within `WIFI_CONNECT_TIMEOUT_MS = 10 s`, flush
  energy and `ESP.restart()`. The failure counter is `RTC_DATA_ATTR`, so it
  survives a software restart but resets on a real power cycle — and it is capped
  at `WIFI_MAX_BOOT_FAILURES = 10` before dropping to AP mode. That cap is what
  stops a device with stale credentials from reboot-looping forever.
- **AP mode**: soft AP `ESP32-Elec-Counter` / `configure123` at 192.168.4.1. A
  `DNSServer` on :53 does the captive-portal wildcard redirect. The portal pages
  themselves are served by the **same** `AsyncWebServer` on :80 as the WebSocket
  (an earlier split between `WebServer` and `AsyncWebServer` double-bound the
  port). The `/` and `/save` handlers re-check `isApMode()` at request time, so
  a later STA transition 404s cleanly instead of serving a stale form.
- **RSSI** refreshes every 5 s; three consecutive `WL_CONNECTED` failures drop
  back to `WIFI_CONNECTING`.

### Eco mode

Every 2 s, `networkTask` computes "is anyone watching":

```
watched = wsServer.clientCount() > 0 || fbBridge.cloudWatched() || otaInProgress
```

If not watched, `WiFi.setSleep(true)` and the cloud push interval drops from 1 s
to 10 s (`FIREBASE_ECO_PUSH_INTERVAL_MS`). Sensing on Core 1 always runs
full-rate — eco only affects radios and the cloud, never measurement. The AP
never sleeps (it must keep beaconing).

## B.9 Three Command Paths, One Engine

There are three ways in, and they converge:

```
  serial 115200 ──► handleSerialCommand() ─┐
  WebSocket /ws ──► handleCommand() ───────┼──► processCommand()  ──► ConsoleHandler
  RTDB /commands ──► pollCommands() ───────┘      (JSON verbs)         (text verbs)
```

- **`processCommand()`** (`command_processor.cpp`) parses the dashboard's JSON
  verbs: `set_name`, `reset_counter`, `test_inject`, `set_voltage_cal`,
  `set_current_cal`, `set_monthly_kwh`, `set_noise_floor`, `set_lpf`,
  `set_rms_samples`, `set_ntfy_topic`, `set_ntfy_enabled`, `reset_ch_cal`,
  `reset_ch_to_default` / `reset_channel_names`, `reset_nvs_defaults`,
  `test_force_rollover`, and `console`.
  It is a hand-rolled `indexOf` scanner, not a JSON library — which is why the
  code carries hand-written escape handling (`jsonUnescape`,
  `extractJsonString`).
  **Every handled verb ends with `nvs->commit()`.**
- **`ConsoleHandler`** is a *text* command engine (the serial vocabulary) that
  captures output into a `String` instead of printing to `Serial` — that is
  precisely what lets the web console reuse it. `{"cmd":"console","line":"..."}`
  routes into it. Output is capped at `MAX_OUTPUT = 3072` bytes so a runaway
  command cannot blow up a WebSocket frame or an RTDB write.
- The serial path special-cases `test led` to run the *blocking* version
  (serial has no latency constraint), while the web path always defers.

### Deferred (blocking) commands

`test led`, `nvs_debug`, and `reboot` set `pendingDefer` and return an
acknowledgement immediately; Arduino's `loop()` on Core 1 calls
`runDeferred()` where blocking is safe (no network task to stall, no WDT risk).
`test led` then runs as a non-blocking state machine driven by
`ConsoleHandler::loop()`.

> `setwifi connect/save` and `clearwifi` are **not** deferred, despite what
> `console_handler.h` and `index.html` say. `exec()` dispatches them inline, and
> `cmdSetWifi()` with `connect`/`save` synchronously does
> `flushEnergy() → commit() → delay(100) → ESP.restart()`. See Part C.3.

## B.10 One Schema, Two Transports

`buildSystemJson()` (in `firebase_bridge.cpp`) is the **single** serialiser for
system state. `WebSocketServer::buildJson()` just calls it. So the LAN WebSocket
payload and the cloud `/latest` snapshot are byte-identical by construction —
the dashboard has one `updateDashboard()` path for both.

Snapshot highlights: `v` voltage, `uptime`, `wifi`/`rssi`/`ap`, calibration
(`voltageCalibration`, `currentCalibration[]`, `rmsSamples`, `noiseFloor[]`,
`lpfAlpha[]`), auto-zero state (`azActive`, `azChannel`, `azProgress`,
`azQueue[]`), `firmwareVersion`, `epoch`, `lastMonth`, `ntfy{topic,enabled}`,
`ch[]` (6 × name/current/watts/VA/PF/kWh/status/limit), and the last 10
`events[]`. Field names are deliberately short.

A stale Firebase error is appended as `fbErr` so a failing push is visible in
the UI rather than silent.

## B.11 Cloud Bridge Details

| Constant | Value | Meaning |
|---|---|---|
| `FIREBASE_PUSH_INTERVAL_MS` | 1000 | `/latest` push when watched |
| `FIREBASE_ECO_PUSH_INTERVAL_MS` | 10000 | `/latest` push when unwatched |
| `FIREBASE_COMMAND_POLL_MS` | 1000 | `/commands` poll |
| `FIREBASE_VIEWER_POLL_MS` | 5000 | `/viewers` poll |

**Two dedicated `FirebaseData` connections — `fbdo` and `fbCmd`.** This is the
single most important implementation detail in the bridge. The Firebase ESP
client multiplexes one TLS session per `FirebaseData`; a failing or empty GET on
the *publish* connection tears that session down and forces a fresh ~1.3–1.9 s
handshake on the next push. Since `/commands` is empty most of the time, sharing
one connection would mean a handshake on nearly every publish. Splitting them
keeps the push path warm.

Consequently **a missing node is normal, not an error**: `pollCommands()` and
`pollViewers()` both check `dataType() == "null"` and return quietly, so they
never trigger the teardown. A genuine error is logged at most every 10 s.

**Auth**: user email/password (service-account style) from
`firebase_config.h`. `configured()` validates the shape of every credential
(`AIza` prefix, contains `@`, no `PASTE_` placeholders) and, if incomplete,
prints one warning and disables Cloud mode instead of failing opaquely later.

**Command execution**: `pollCommands()` iterates the `/commands` object and
calls `processCommand()` per entry, then writes any text response to
`/console/<key>` and **deletes the command node**. Deletion is what makes it
fire-and-forget and prevents re-execution.

> `FirebaseJson::iteratorGet` yields nested members as well as top-level keys.
> The loop only acts on `depth == 0` entries. Without that guard, a pushed
> object's bare `cmd` field would be reconstructed as `{"cmd":"..."}` with no
> `ch`, and per-channel commands would silently act on *every* channel. The same
> class of bug is why `reset_ch_to_default` now refuses to run without a valid
> `ch` instead of wiping all six channels.

**Viewer presence** (`/viewers`): the dashboard writes
`{ts: ServerValue.TIMESTAMP}` under `/viewers/<clientId>` every 15 s and removes
it on `onDisconnect` + `pagehide`/`beforeunload` + tab hide. The device polls
every 5 s; a non-empty node sets `viewerPresent`, which relaxes the push interval
to 1 s and disables eco sleep. Transient read errors keep the last known state so
eco does not flap.

## B.12 RTDB Data Model & Security

```
/devices                                  .read: true          (board picker)
/devices/<deviceId>                       .read: true, .write: false
/devices/<deviceId>/latest                .read: true,  .write: admin
/devices/<deviceId>/commands              .read: admin, $cmd .write: admin
/devices/<deviceId>/console               .read: admin, $out .write: admin
/devices/<deviceId>/ota                   admin read+write
/devices/<deviceId>/firmware              admin read+write
/devices/<deviceId>/viewers/$v            .write: true, .validate: hasChildren(['ts'])
```

`deviceId` is `"esp-" + hex(low 24 bits of the efuse MAC)`
(`src/utils/device_id.cpp`) — stable across reflashes and unique per chip, so
multiple boards share one database without clobbering each other.

Design notes:

- The **parent** `$device` node has `.write: false`, which is what makes the
  per-child grants meaningful — a child grant cannot be escalated to the parent.
- `/commands` and `/console` grant `.write` **per child key**, which is exactly
  what `.push()` needs and no more.
- `/viewers` is the only unauthenticated write, deliberately: presence must work
  for signed-out guests. The `hasChildren(['ts'])` validation pins the payload
  shape. Worst case a stranger keeps the board awake.
- `/latest` is world-readable, so a guest dashboard works without signing in.
- The admin test is the email expression `auth.token.email == 'heng.xiao.hour@gmail.com'
  || auth.token.email == 'esp32-counter@chenla.com'`, **inlined at all seven
  privileged rules**. There is no `isAdmin()` function (Part C.5).

**Auth scope, stated honestly:** in Cloud mode the rules are the real enforcement.
In **Local (WebSocket) mode there is no server-side auth on the ESP32 at all** —
the admin check is UI-level only, so Local is trusted-LAN-only. Demo mode is
intentionally fully open.

## B.13 Dashboard (PWA)

`frontend/script.js` (~1.5k lines) holds all UI logic; `index.html` is the
skeleton. Firebase compat SDK 10.12.2 is loaded from gstatic.

**Connect panel** — one `#connMode` dropdown (Cloud / Local / Demo) plus one
`Launch` button that routes to `connectCloud()` / `connectLocal()` /
`startDemoMode()`. Cloud is the default. Only the relevant extra field is shown
(IP row for Local, device picker for Cloud). The session is persisted in
`localStorage` and auto-reconnect runs on load.

| Mode | Transport | RTDB paths |
|---|---|---|
| **Cloud** | firebase SDK | subscribes `/latest` + `/console`; pushes `/commands`; heartbeats `/viewers` |
| **Local** | `ws://<ip>/ws` | none |
| **Demo** | `setInterval` mock, no I/O | none |

**One board, many clients.** `normalizeSnapshot()` runs `toArray()` over
`ch`, `currentCalibration`, `noiseFloor`, `azQueue`, `lpfAlpha` and `events`,
because RTDB stores arrays as objects with numeric string keys. Forgetting this
is the classic RTDB-array bug.

**Auth** — `initAuth()` applies the auth state *synchronously* before Firebase
initialises so admin controls never flash for non-admins on reload. Admin is a
client-side email allowlist (`FB_CONFIG.adminEmails`). Gating is applied by
`sendCommand()` rejecting non-admins, `requireAdmin()` guards on the modals, and
a `role-downgrade` check force-closes an open channel modal. The `/console`
listener is attached **only** when admin — guests lack read permission there, so
attaching anyway would spam `PERMISSION_DENIED`.

Connection/viewing is never gated; only command-sending is. A 10 s staleness
watchdog flips the status banner to "Offline — no data" if `/latest` stops.

**Structure** — connect panel, then a sidebar (Dashboard / Analytics / History /
Settings) + mobile nav, a status bar (voltage, total power/current, date, device
time), an OTA progress panel, and a channel edit modal. Settings holds Account,
Connection, ntfy, System Calibration, Device Console, and About — the last four
of which are `admin-only`.

**Service worker** (`sw.js`) — network-first with cache fallback. It precaches
the app shell, passes through non-GET and cross-origin requests (so the gstatic
Firebase scripts and all RTDB traffic bypass it), returns the network response
when `res.ok`, and on failure falls back to the cache, then to `./index.html` so
offline navigations still resolve. `firebase.json` additionally forces
`Cache-Control: no-cache` for everything, `/sw.js` and `/manifest.json`. Cache
invalidation is by bumping `CACHE_NAME` (`esp32-counter-v12`) and the
`?v=` query strings on `style.css` / `script.js` / `config.js`.

**Demo mode caveat:** `sendCommand()` mutates local state for `set_name`,
`set_monthly_kwh` and `reset_counter` *before* the admin check, so demo
mutations work for guests by design.

## B.14 OTA

`ArduinoOTA` under hostname `esp32-elec-counter`, begun only after WiFi is up.
The LED goes solid blue while `isInProgress()` and the dashboard shows
`otaProgress`/`otaPercent`/`otaVersion`. Partition scheme must be
`No FS 4MB (2MB APP with OTA)` — the firmware with the Firebase client does not
fit the default 1.25 MB app partition. The RFQString OTA panel is hidden unless
the payload carries `ota: true`.

## B.15 Timing Budget

| Cadence | Value | Where |
|---|---|---|
| Sensor cycle | 80 ms | `SENSOR_CYCLE_INTERVAL_MS` |
| WebSocket broadcast | 150 ms | `WS_UPDATE_INTERVAL_MS` |
| Network task tick | 20 ms | `vTaskDelayUntil` in `networkTask` |
| Firebase task tick | 50 ms | `vTaskDelayUntil` in `firebaseTask` |
| Cloud push (watched / eco) | 1 s / 10 s | `FIREBASE_*_PUSH_INTERVAL_MS` |
| `/commands` poll | 1 s | `FIREBASE_COMMAND_POLL_MS` |
| `/viewers` poll | 5 s | `FIREBASE_VIEWER_POLL_MS` |
| ntfy retry | 10 s | `NtfyNotifier::RETRY_INTERVAL_MS` |
| NVS energy save | ~5 s | `sensorTask` |
| Eco decision | 2 s | `networkTask` |
| Auto-zero | 2 of 32 batches per 80 ms cycle | `AZ_BATCHES_PER_CYCLE` |
| WiFi connect timeout / retry | 10 s / 10 s | `WIFI_*` |
| AP fallback after retries | 5 retries | `WIFI_MAX_RETRIES` |
| Boot-failure reboot cap | 10 | `WIFI_MAX_BOOT_FAILURES` |

## B.16 Console Vocabulary

Available identically from the serial port and the web console:

```
help / ?              status              debug
ch <N>                cal                 info               wifi
buzz <N>              inject <ch> <kwh>   reset <N>          reset_name [N]
test led              rms_samples <N>     curr_cal <ch> <val>
auto_zero <ch>        volt_cal <val>      setwifi sta|ap|auto
setwifi ssid <name>   setwifi pass <pwd>  setwifi connect    clearwifi
nvs_debug             reboot
```

`status` and `debug` **toggle** live serial streams via the `g_statusStream` /
`g_debugStream` gates in `log_gate.h` (both default OFF, so a fresh boot prints
only the WiFi/server block). `STATUS_LOG` is for operational transitions (eco
sleep, viewer presence, rollover, auto-zero, ntfy); `DEBUG_LOG` is for developer
diagnostics (NVS writes, `[CMD]` traces, Firebase/ntfy errors).

`test led`, `nvs_debug`, `reboot` are deferred; the rest run inline.

---

# Part C — Documentation Drift

Found while writing Part B. Part B reflects the code; these older statements are
wrong or stale.

### C.1 `src/sensor/` is dead code

`current_sensor.{h,cpp}` and `voltage_sensor.{h,cpp}` are **not included by any
file in the project** (verified by grep — the only other hits are in the old
tree listing). All ADC sampling and RMS math lives in
`PowerCalculator::collectSamples()` / `computeAll()`. The old tree comment
"6-ch ADC sampling + RMS calculation" describes a module that no longer runs.
Either delete the directory or re-document it as legacy. Flagged, not changed —
deletion needs your call.

### C.2 README: trip LED behaviour

`README.md` says a tripped channel shows a "RED LED blink". It does not.
`updateLED()` only ever selects blue (OTA), red (WiFi down), or green. Blink
modes are reachable solely from `test led`, as `status_led.h` itself notes.
Trips are signalled by the **buzzer**, the channel `status` field, the event log,
and the ntfy push — not the LED.

### C.3 Deferral claims for `setwifi` / `clearwifi`

`console_handler.h:18-21` and `index.html:263` both state that `setwifi
connect/save` and `clearwifi` are deferred / "serial-only". Neither is true:
`exec()` dispatches them inline, and `cmdSetWifi()` with `connect`/`save` calls
`ESP.restart()` synchronously. All five commands are reachable from the web
console today.

### C.4 Dead constants in `config.h`

`ENERGY_UPDATE_INTERVAL_MS` (1000) and `SAMPLES_PER_CYCLE` (500) are defined but
referenced nowhere in code. Energy integrates on every 80 ms sensor cycle using
measured elapsed time, so the former is misleading. The latter is documentation
of intent (≈500 samples per 50 Hz cycle at 25 kHz).

### C.5 `isAdmin()` does not exist in `database.rules.json`

`README.md:44` and `config.example.js:15` both direct the reader to enforce admin
in `database.rules.json` → `isAdmin()`. There is no such function — the email
expression is inlined verbatim at all seven privileged rules. Worth extracting to
a real `isAdmin` function so the list lives in exactly one place.

### C.6 `esp32-counter@chenla.com` is unexplained

That identity is granted full admin in the rules but appears nowhere in `src/`
or `frontend/config.example.js`. The firmware authenticates with
user email/password from `firebase_config.h`, so it is presumably that account —
but the repo does not say so, and the shipped `adminEmails` list does not include
it, so a signed-in dashboard can never present it.

### C.7 Device-ID zero padding

`deviceId()` formats the low 24 bits with `String(chip, HEX)`, which does **not**
zero-pad, despite `device_id.h` saying "6 hex chars". A board whose low 24 bits
start with a zero byte yields a 1–5 character id. Harmless (it is just a key) but
it makes hand-copying the id into `frontend/config.js` `deviceId` error-prone.

### C.8 `.firebaserc` / `scripts/` vs `tools/`

The old tree listed only `tools/hosting_deploy.py`; the repo also has
`tools/firebase_rest.py`, and `scripts/setup.py` + `scripts/deploy.py` were
absent from the tree. Tree in Part A now matches the filesystem.

### C.9 `doc/opencode_agent/AGENTS.md` hosting model is stale

It states the UI is hosted externally and the ESP32 "only runs the WebSocket
server — no HTTP file serving". The first half is still true, but the device now
also serves the AP captive portal from the same :80 server when in AP mode.

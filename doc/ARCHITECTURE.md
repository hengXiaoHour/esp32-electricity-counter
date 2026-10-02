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
├── .gitignore
├── README.md                        # Setup instructions
├── design.md                        # Dashboard visual style + design decisions
│
├── doc/                             # Documentation
│   ├── ARCHITECTURE.md              # THIS FILE
│   ├── .architecture.lock.json      # Machine-readable lock (do not edit)
│   ├── esp32s3-electricity-counter-prompt.md  # Original build prompt
│   └── opencode_agent/              # Agent context (hand-maintained)
│       ├── AGENTS.md               # traps + traps-not-in-the-code; points here
│       └── lessons.md               # hard-won knowledge not derivable from code
│
├── src/
│   ├── config.h                     # Pins, #define constants, shared structs/enums
│   ├── core/
│   │   ├── power_calculator.h/.cpp  # ALL ADC sampling + RMS + P/PF/kWh math
│   │   └── limit_manager.h/.cpp     # Budget state machine, buzzer, events, rollover
│   ├── network/
│   │   ├── wifi_manager.h/.cpp      # Soft AP + captive DNS. No station interface.
│   │   ├── websocket_server.h/.cpp  # AsyncWebSocket + serves the PWA from flash
│   │   ├── web_assets.h             # GENERATED — frontend/ baked into PROGMEM
│   │   ├── system_json.h/.cpp       # buildSystemJson() — the single serialiser
│   │   ├── time_sync.h/.cpp         # Clock borrowed from the connected browser
│   │   ├── auth_gate.h/.cpp         # Admin-PIN decision logic (no Arduino deps)
│   │   ├── command_processor.h/.cpp # JSON command parser + PIN enforcement
│   │   ├── console_handler.h/.cpp   # Text console engine shared by serial + web
│   │   └── ota_handler.h/.cpp       # ArduinoOTA wrapper
│   ├── ui/
│   │   ├── status_led.h/.cpp        # WS2812 (with R/G swap workaround)
│   │   └── buzzer.h/.cpp            # Non-blocking beep pattern driver
│   └── utils/
│       ├── nvs_manager.h/.cpp       # Preferences wrapper (all persistence)
│       └── log_gate.h               # STATUS_LOG / DEBUG_LOG serial stream gates
│
├── frontend/                        # Dashboard PWA — source of truth, baked into
│   ├── index.html                     # flash by scripts/embed_web.py
│   ├── style.css
│   ├── script.js                    # WS client, PIN, notifications, all UI logic
│   ├── manifest.json
│   ├── sw.js                        # Service worker (inert over plain http)
│   └── icons/
│
├── scripts/
│   ├── embed_web.py                 # frontend/ -> src/network/web_assets.h
│   ├── test_auth_gate.c             # Host unit tests for the PIN gate
│   ├── mock_device.py               # Mock board: serves frontend/, speaks /ws
│   ├── e2e_aponly.js                # Playwright run of the real page
│   ├── check_deadcode.py            # Fails if dead code/assets reappear
│   ├── verify_all.sh                # Every gate, one command
│   └── patch_async_tcp.py           # AsyncTCP 1.1.4 patches (see README)
│
│ x firebase.json / database.rules.json / .firebaserc  -- deleted, no cloud
│ x tools/                            -- deleted, held the RTDB + hosting helpers
│ x frontend/config.js                -- deleted, held the Firebase web config
│ x src/utils/device_id.*             -- deleted, only ever keyed RTDB paths
│ x src/sensor/*                      -- deleted, dead since PowerCalculator
│ x scripts/setup.py                  -- deleted, a Firebase config wizard
│ x .workflow/{active,PLAN,RESEARCH}  -- deleted, completed 2026-08 tasks
│ x .workflow/VERIFICATION.log        -- deleted, logged the ripped-out relay pair
│ x opencode.json                     -- deleted, an unrelated Stitch MCP config
```

The project root has no agent scratch directory. Per-task working files
(`PLAN.md`, `RESEARCH.md`, `active.json`) are gitignored and are expected to be
deleted when the task completes; the durable record is git itself — every
change lands as a commit message, which is why nothing here needs a changelog
file that has to be maintained by hand.

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

One piece. The board *is* the network, the web server and the application.

```
   ┌───────────────────────────────────────────────────────────────┐
   │  Phone / laptop                                            │
   │    joins WiFi  "ESP32-Elec-Counter"                          │
   └───────────────────────────┬───────────────────────────────────┘
                               │  soft AP.  DNSServer:53 answers EVERY
                               │  hostname with 192.168.4.1, so any URL —
                               │  including the OS's captive-portal probe —
                               │  lands on the dashboard.
                               ▼
   ┌───────────────────────────────────────────────────────────────┐
   │  ESP32-S3  ·  AP-only, always                                │
   │                                                               │
   │  AsyncWebServer :80                                          │
   │    /                 -> the whole PWA, served from flash     │
   │    /style.css /script.js /manifest.json /sw.js /icons/*      │
   │    /ws               -> live JSON snapshot + commands        │
   │  DNSServer  :53       -> captive-portal wildcard             │
   │  ArduinoOTA :3232                                           │
   │                                                               │
   │  Core 1: sensing          Core 0: transport                   │
   └───────────────────────────────────────────────────────────────┘
```

**There is no station interface and no cloud.** Nothing the board does requires
an upstream network, and the dashboard it serves is compiled into its own flash.

### Why the board serves the dashboard

This replaced a Firebase Hosting deployment, and the reason is mechanical, not
aesthetic:

- **Same origin.** The page is `http://192.168.4.1`, so `ws://192.168.4.1/ws` is
  same-origin. The hosted dashboard was `https://…`, and a browser refuses to
  open a `ws://` socket from a secure page as **mixed content**. The old
  `ap_portal.h` said so in its own header comment, which is why a read-only
  table page had to exist as a fallback.
- **No captive-DNS conflict.** While joined to the AP, every hostname resolves
  to `192.168.4.1`, so a hosted UI cannot even load reliably without falling
  back to mobile data.
- **It works offline**, at LAN latency, with the router unplugged.
- Flash allows it: 10 assets, ~101 KB, into a partition that freed ~376 KB
  when the Firebase client library was removed.

**Trade-off:** the counter can no longer be read from outside the local WiFi.
That is the point of the change, and it is a real loss.

**Data is one-way per frame.** The board broadcasts the same JSON snapshot to
every connected client at 150 ms, and commands travel back over the same socket.
`AsyncWebSocketClient` has no usable per-client state field, so the admin PIN is
sent with every mutating frame rather than negotiated once (see B.9).

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

## B.3 Runtime: Two Tasks, Two Cores

`setup()` creates the tasks; `loop()` stays nearly empty on purpose.

| Task | Core | Prio | Stack | Period | Responsibility |
|---|---|---|---|---|---|
| `networkTask` | 0 | 2 | 8192 | 20 ms | AP + captive DNS, dashboard server, WebSocket broadcast, serial RX, OTA |
| `sensorTask` | 1 | 2 | 8192 | 80 ms | ADC sampling, power math, limit check, buzzer, LED, NVS energy save |

Plus Arduino's default `loopTask` on Core 1, which only drives
`consoleHandler.runDeferred()` / `consoleHandler.loop()`.

**`firebaseTask` is gone.** It existed (priority 1 on Core 0) purely to keep the
Firebase client's blocking TLS handshake and 1 s `/latest` push away from the
150 ms WebSocket broadcast. With no cloud there is nothing to isolate, so Core 0
has exactly one task.

**`networkTask` is now both cores' only writer of network state**, and the AP is
started once in `setup()` before the task exists. `softAP()` returning *is* the
readiness signal: `startServer()` and `otaHandler.begin()` are gated on
`wifiMgr.isReady()` rather than on a wait-for-IP handshake.

**`ArduinoOTA.begin()` must still wait for the AP to hold an IP** — started
earlier it silently never listens.

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
  `broadcastData()` (which is the only caller of `buildSystemJson()` now).
  `ConsoleHandler::flushEnergy()` also takes it: `commit()` is
  `prefs.end(); prefs.begin()`, which is *not* thread-safe against
  `sensorTask`'s 5 s save. With STA gone, `reboot` is the only restart path, so
  that flush is the last chance to get counters into flash.
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

- **First crossing only** (`tripNotified[ch]` latch): append a forensic event.
  This is the anti-flood latch. The event is what the dashboard turns into a
  Web Notification — there is no server-side push left, so the buzzer, the
  channel `status` field and this event are the whole alarm path.
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

The RAM ring holds 50 events and is wiped by any restart — which is exactly when
you most want to know whether the trip was recent. So the last
`FORENSIC_KEEP = 10` events are also written to NVS on every *critical* event
(trip, manual reset, rollover, energy inject, boot). That is a few writes per
day, negligible for flash wear. `setup()` restores them into `SystemData` before
the first broadcast, so the trail is visible the moment a dashboard connects.

## B.7 Persistence (NVS)

`NVSManager` wraps Arduino `Preferences` and is the only writer to flash.
**`commit()` is required** for anything to survive reboot, and the code is
disciplined about it: every mutating command ends with `nvs->commit()`, rollover
commits immediately, and `flushEnergyToNvs()` runs as a pre-restart hook.

| Group | Keys |
|---|---|
| Legacy (written by older firmware, now unread) | `wifi_ssid`, `wifi_pass`, `wifi_mode` — erase with `clearwifi` |
| Auth | `admin_pin` (default `1234`, plaintext) |
| Per channel | name, monthly kWh limit, current cal, noise floor, LPF alpha, energy kWh |
| Global | voltage cal, rms samples, last billing month, forensic events |

`ntfy_topic` / `ntfy_enable` may still be present from older firmware and are now
ignored.

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

## B.8 Connectivity: an Access Point, Nothing Else

`WiFiManager` is a single-state machine now. `begin()` takes no arguments, does
not read NVS, and calls `startAPMode()`.

```cpp
WiFi.mode(WIFI_AP);
WiFi.softAP("ESP32-Elec-Counter", "configure123");
WiFi.setTxPower(WIFI_POWER_21dBm);
WiFi.setSleep(false);              // an AP must keep beaconing
WiFi.softAPConfig(192.168.4.1, 192.168.4.1, 192.168.4.1);
dnsServer.start(53, "*", apIP);    // every hostname -> the dashboard
```

`loop()` does exactly one thing: `dnsServer.processNextRequest()`.

**Removed with the STA path**, and each was load-bearing in a way worth
recording:

| Gone | Why it no longer applies |
|---|---|
| STA connect, retry ladder, `WIFI_MAX_RETRIES` | there is no network to join |
| Connect-timeout reboot failsafe + `RTC_DATA_ATTR` failure counter | nothing to time out |
| Link-flap detection, `WL_CONNECTED` polling | there is no link |
| RSSI sampling | an AP does not report one |
| Modem-sleep eco mode | an AP must keep beaconing; see below |
| `setwifi` console verb, `/save` portal handler | no credentials to store |
| `isConnected() \|\| isApMode()` | two-way test for a one-way state; now `isReady()` |

**Stored credentials are left alone.** Old firmware wrote `wifi_ssid` /
`wifi_pass` into NVS. Nothing reads them now, and they are deliberately *not*
wiped automatically — a downgrade to an older build still finds them. `clearwifi`
exists purely to erase them.

### Eco mode is gone

The old design slept the modem and slowed cloud pushes to 10 s when nobody was
watching (`wsServer.clientCount() || fbBridge.cloudWatched() || otaInProgress`).
Both inputs are gone and the mechanism is meaningless: a station link can sleep,
an access point cannot. Sensing on Core 1 was never affected by eco and still
runs full-rate.

## B.9 One Command Path, One Gate

```
  serial 115200 ──► handleSerialCommand() ──┐
  WebSocket /ws ──► handleCommand() ────────┼──► auth_check() ──► processCommand()
                                             │      (PIN)            │
                                             │                       ├──► ConsoleHandler (text)
                                             │                       └──► NVS / live state (JSON)
```

`processCommand()` parses the dashboard's JSON verbs: `set_name`,
`reset_counter`, `test_inject`, `set_voltage_cal`, `set_current_cal`,
`set_monthly_kwh`, `set_noise_floor`, `set_lpf`, `set_rms_samples`,
`reset_ch_cal`, `reset_ch_to_default` / `reset_channel_names`,
`reset_nvs_defaults`, `test_force_rollover`, `set_pin`, `set_time`,
`verify_pin`, and `console`.

It is a hand-rolled `indexOf` scanner, not a JSON library — which is why the
code carries hand-written escape handling. Every mutating verb ends with
`nvs->commit()`.

### The admin gate

The gate runs **before any verb touches state**, so a newly added mutating verb
is protected by default rather than by remembering to add a check.

```cpp
char verb[AUTH_MAX_VERB];
if (!auth_check(msg, nvs->loadPin().c_str(), verb, sizeof(verb))) {
  *authRejected = true;          // -> {"type":"auth","ok":false} to that client
  return false;
}
```

The decision itself lives in `src/network/auth_gate.cpp`, which has **no Arduino
dependency** — so it is unit-tested on the host by `scripts/test_auth_gate.c`
(47 assertions, mutation-checked). A security check that can only be exercised by
flashing hardware is a check nobody runs.

| Verb | PIN? | Why |
|---|---|---|
| `set_time` | no | every viewer sends it on connect; it is what gives the board a clock at all |
| `verify_pin` | no | its whole job is to answer the question |
| everything else | **yes** | any state change, including `console` and `set_pin` |

**Stateless by design.** `AsyncWebSocketClient` has no usable per-client state
field, so the PIN is attached to every mutating frame rather than negotiated into
a session token. There is no token to forge, revoke, or desynchronise.

Rejected frames get a **distinct** `{"type":"auth","ok":false}` reply, separate
from `handled == false`. The dashboard cannot otherwise tell "you are not
allowed" from "I did not understand that", and since the board is the authority,
the UI must not have to guess. On receiving it the page drops back to read-only
and forgets the cached PIN — so changing the PIN on the device revokes open tabs.

### The browser lends the clock

`set_time` is the reason this section is not just about authorisation.

```cpp
int ti = s.indexOf("\"t\":");
int64_t epoch = s.substring(ti + 4).toInt();
timeSync.setTimeFromBrowser(epoch);
```

`TimeSync` keeps the last accepted sync in `RTC_DATA_ATTR`, which survives
`ESP.restart()` but not a power cut. `begin()` restores it and advances it by
the elapsed `millis()`, so the board has a plausible clock at boot with no phone
attached. `setTimeFromBrowser()` rejects epochs outside
[2021-01-01, 2100-01-01] and anything more than 180 days from what the board
already believes — a bad frame must never be able to roll the billing month.

### Deferred (blocking) commands

`test led`, `nvs_debug` and `reboot` set `pendingDefer` and return an
acknowledgement immediately; Arduino's `loop()` on Core 1 calls `runDeferred()`,
where blocking is safe. `test led` then runs as a non-blocking state machine.

> `setwifi` is gone. `clearwifi` runs inline and does nothing but erase stale
> credentials — it never rebooted, contrary to what the old notes said.

## B.10 One Schema, One Transport

`buildSystemJson()` (`src/network/system_json.cpp`) is the **single** serialiser.
It used to live inside `firebase_bridge.cpp` because the cloud push was its
first caller; with the cloud gone it was promoted to its own translation unit
rather than deleted, and `WebSocketServer::buildJson()` is now its only caller.
Burying the wire format inside a deleted bridge is how it would have ended up
duplicated.

Snapshot highlights: `v` voltage, `uptime`, `wifi`/`rssi`/`ap`, calibration
(`voltageCalibration`, `currentCalibration[]`, `rmsSamples`, `noiseFloor[]`,
`lpfAlpha[]`), auto-zero state (`azActive`, `azChannel`, `azProgress`,
`azQueue[]`), `firmwareVersion`, `epoch`, `lastMonth`, `time{ok,age}`, `ch[]`
(6 × name/current/watts/VA/PF/kWh/status/limit), and the last 10 `events[]`.
Field names are deliberately short: the string is rebuilt and pushed to every
connected browser ~6–7 times a second.

`time{ok,age}` is read straight from `TimeSync`, not through `SystemData`: it is
owned by the network layer, needs no mutex, and must be visible when it has
*never* been set.

The old `fbErr` field is gone with the bridge that appended it.

## B.11 Serving the Dashboard

`WebSocketServer::serveAsset()` answers the catch-all from the generated
`WEB_ASSETS[]` table.

```cpp
server->onNotFound([](AsyncWebServerRequest *request) {
  if (WebSocketServer::serveAsset(request)) return;
  request->send(404, "text/plain", "Not found");
});
```

`findAsset()` strips any `?v=` cache-buster, maps `/` to `/index.html`, and then
matches **exactly**. No prefix matching, no directory walking: this is a fixed
set of ten files, and "serve whatever the path walks to" is how a device turns
into an open file server.

The response uses the explicit-length `beginResponse(code, mime, uint8_t*, len)`
overload. The `char*` overload measures with `strlen()`, which would truncate
every PNG at its first `0x00` byte. `Cache-Control: no-cache, no-store,
must-revalidate` is set on all of them — the assets live in flash, so there is
nothing to revalidate against.

### Generating the assets

`scripts/embed_web.py` bakes `frontend/` into `src/network/web_assets.h`: text
assets as PROGMEM raw string literals opened directly on the first content byte,
binary assets as byte arrays, sizes from `sizeof()` so the table cannot drift
from the bytes. Ten files, ~101 KB.

```
python3 scripts/embed_web.py                       # regenerate
python3 scripts/embed_web.py --check                # stale? parses the header back
python3 scripts/embed_web.py --verify               # bytes match frontend/
python3 scripts/embed_web.py --verify-binary <bin>  # ...and reach the flash image
```

`--verify` deliberately does not trust the generator: it parses the emitted C++
back into bytes and compares against `frontend/`. An early draft opened every
raw literal with `R"rawliteral(` + newline, which silently prepended one byte to
all five text assets — which is exactly what `--verify` exists to catch.

## B.12 The Dashboard

`frontend/script.js` holds all UI logic; `index.html` is the skeleton. There
are **no external scripts** — the Firebase SDK tags are gone, and a gate
asserts it.

| Concern | Old | New |
|---|---|---|
| Transport | Cloud (RTDB) or Local (typed IP) | one WebSocket to `location.host` |
| Connect UI | mode dropdown + IP field + board picker | none; `init()` connects at once |
| Identity | Google sign-in + RTDB rules | admin PIN, enforced on the ESP32 |
| Offline | no | works with the router unplugged |
| Trip alert | ntfy.sh push | Web Notification while the page is open |

Because the page is served by the board, `location.host` *is* the board — there
is no IP to type. `showDashboard()`/`showConnectPanel()` survive only as the
disconnected notice.

**Clock lending.** `sendTime()` fires on socket open and every 10 minutes.
`renderClock()` shows the board's reported state, including the case that
matters: *not set — monthly reset will not fire*.

**Trip notifications.** `notifyOnTrip()` keys on *which* channels are tripped,
not on arrival, so a snapshot arriving 6× a second does not re-notify, but a new
channel tripping does. `tag` replaces the previous notification instead of
stacking them.

**PIN handling.** The PIN is cached in `sessionStorage` (per tab, not
persistent) and re-validated against the board on every connect — the board is
the authority, not the cache. `verifyPin()` resolves **false** on timeout: an
unverified PIN is not a verified one.

`applyPinState()` also force-closes an open channel modal when admin is lost.

**Demo mode** survives as `?demo=1` — mock data, no socket. Its only remaining
purpose is previewing the dashboard on a machine that cannot join the board's
WiFi.

### Service worker

`sw.js` is retained and correct, but **inert in practice**: service workers
require a secure context, and `http://192.168.4.1` is not one. So there is no
true PWA install — the browser offers "Add to Home Screen", which is a
shortcut. Serving HTTPS from the board would require generating a self-signed
certificate, which is a separate piece of work.

## B.13 OTA

`ArduinoOTA` under hostname `esp32-elec-counter`, begun from `networkTask` once
`wifiMgr.isReady()`. Over the board's own AP the IDE may not discover it
automatically; entering `192.168.4.1` as the network port works.

The LED goes solid blue while `isInProgress()` and the dashboard shows
`otaProgress`/`otaPercent`/`otaVersion`. Partition scheme must remain
`No FS 4MB (2MB APP with OTA)`.

## B.14 Timing Budget

| Cadence | Value | Where |
|---|---|---|
| Sensor cycle | 80 ms | `SENSOR_CYCLE_INTERVAL_MS` |
| WebSocket broadcast | 150 ms | `WS_UPDATE_INTERVAL_MS` |
| Network task tick | 20 ms | `vTaskDelayUntil` in `networkTask` |
| Clock re-lend | 10 min | `timeSyncTimer` in the dashboard |
| ntfy retry | — | gone with ntfy |
| NVS energy save | ~5 s | `sensorTask` |
| Eco decision | — | gone with eco mode |
| Auto-zero | 2 of 32 batches per 80 ms cycle | `AZ_BATCHES_PER_CYCLE` |
| WiFi connect timeout / retry | — | gone with STA |
| AP fallback after retries | — | gone with STA |

The status/debug serial streams print every 2 s / 5 s when enabled.

## B.15 Console Vocabulary

Available identically from the serial port and the web console (the web console
is `{"cmd":"console","line":"..."}` into `ConsoleHandler`):

```
help / ?              status              debug
ch <N>                cal                 info               wifi
buzz <N>              inject <ch> <kwh>   reset <N>          reset_name [N]
---
test led              rms_samples <N>     curr_cal <ch> <val>
auto_zero <ch>        volt_cal <val>      clearwifi
nvs_debug             reboot
```

`status` and `debug` **toggle** live serial streams via the `g_statusStream` /
`g_debugStream` gates in `log_gate.h` (both default OFF, so a fresh boot prints
only the boot block). `STATUS_LOG` is for operational transitions (clock sync,
rollover, auto-zero, NVS restores); `DEBUG_LOG` is for developer diagnostics.

`test led`, `nvs_debug` and `reboot` are deferred; the rest run inline.
Output is capped so a runaway command cannot blow up a WebSocket frame.

---

# Part C — Documentation Drift

Recorded while writing Part B. Part B reflects the code; these older statements
were wrong or stale. Items C.1–C.4 were **fixed** during the AP-only migration;
the rest describe files that no longer exist.

### C.1 `src/sensor/` was dead code — **DELETED**

`current_sensor.{h,cpp}` and `voltage_sensor.{h,cpp}` were included by nothing
(all sampling lived in `PowerCalculator`). Verified by grep, then removed.

### C.2 README: trip LED behaviour — **still true, still wrong**

`updateLED()` only ever selects blue (OTA), red (AP not ready) or green. Blink
modes are reachable solely from `test led`, as `status_led.h` itself notes.
Trips are signalled by the **buzzer**, the channel `status` field, the event
log, and the dashboard notification — not the LED.

### C.3 Deferral claims for `setwifi` / `clearwifi` — **RESOLVED BY DELETION**

`console_handler.h` and `index.html` once stated that `setwifi connect/save` and
`clearwifi` were deferred / "serial-only". Neither was true. Both verbs are now
gone (`setwifi` prints a "board is AP-only" message; `clearwifi` runs inline and
only erases stale credentials).

### C.4 Dead constants in `config.h` — **REMOVED**

`ENERGY_UPDATE_INTERVAL_MS`, `SAMPLES_PER_CYCLE`, `NTFY_HOST`, `NTFY_PORT`,
`WIFI_RETRY_INTERVAL_MS`, `WIFI_MAX_RETRIES`, `AP_FALLBACK_TIMEOUT_MS`,
`WIFI_CONNECT_TIMEOUT_MS` and `WIFI_MAX_BOOT_FAILURES` were all unreferenced
after the migration. Verified with a per-symbol grep before deleting.

### C.5–C.9 — obsolete by deletion

| Old note | Status |
|---|---|
| `isAdmin()` never existed in `database.rules.json` | `database.rules.json` deleted |
| `esp32-counter@chenla.com` was an unexplained admin identity | `firebase_config.h` deleted |
| `deviceId()` does not zero-pad to 6 hex chars | `src/utils/device_id.*` deleted |
| `.firebaserc` / `scripts/` vs `tools/` mismatch | `tools/`, `firebase.json`, `.firebaserc`, `scripts/deploy.py` deleted |
| `AGENTS.md` said the UI is hosted externally and the ESP32 only runs a WebSocket server | superseded by B.1/B.11: the ESP32 serves the UI |

### C.10 Two dead-code lessons worth keeping

Both were found by grep and would have shipped unnoticed:

- `src/utils/device_id.*` became orphaned when the RTDB paths went, and was only
  referenced by its own `.cpp`.
- `src/sensor/*` had been orphaned long before this change, and was still listed
  in the Part A tree as if it were live.

The Part A tree is the thing that made both look intentional. A tree that lists
dead files is worse than no tree.

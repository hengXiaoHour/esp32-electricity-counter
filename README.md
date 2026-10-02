# ESP32-S3 6-Channel AC Electricity Counter

Monitors 6 AC circuits with current sensors, 1 voltage reference, RGB status LED
and an active buzzer. The board is an **access point that hosts its own
dashboard** — no cloud, no router, no internet required.

## How to use it

1. Power on the board.
2. Join the WiFi network **`ESP32-Elec-Counter`** (password `configure123`).
   Both are the factory defaults and can be changed from the dashboard or the
   serial console — see [Changing the network name and password](#changing-the-network-name-and-password).
3. Open **`http://192.168.4.1/`**.

That is the whole setup. The page is served from the board's own flash, so it
works with the router unplugged. Because the phone has already joined the
board's network, opening that address is all it takes — and because the page
and the WebSocket share an origin, live data connects with no configuration,
no login and no mixed-content problems.

To use the dashboard again later, rejoin the network and reload the page.

### Admin PIN

Viewing is always open. Anything that changes the board — channel names and
limits, calibration, counters, the console, rebooting, changing the PIN — needs
the admin PIN (**`1234`** by default; change it in Settings → Admin PIN).

The PIN is checked **on the ESP32**, not in the browser: hiding a button is a
convenience, not a protection. If the PIN is changed on the device while a
dashboard is open, that tab drops back to read-only on its next command.

## Hardware Setup

| Component | Pins |
|---|---|
| CT sensors (ch1-6) | GPIO7, 5, 6, 8, 4, 2 |
| Voltage sensor | GPIO1 |
| Active buzzer | GPIO13 |
| RGB LED (WS2812) | GPIO48 |

Each channel has an independent monthly kWh limit. When a channel reaches 100%
of its limit it trips: the buzzer rings N beeps (N = channel number, ch1 = 1
beep … ch6 = 6 beeps), the channel shows TRIP, an event is logged, and the
dashboard raises a browser notification if it is open.

## Arduino IDE Setup

1. **Board:** Tools → Board → ESP32 Arduino → `ESP32S3 Dev Module`
2. **Partition Scheme:** Tools → Partition Scheme → `No FS 4MB (2MB APP with OTA)`
3. **Flash Size:** 4MB (matches the onboard XMC embedded flash)
4. **PSRAM:** Enabled (2MB PSRAM on this board)

### Required Libraries

| Library | Version |
|---|---|
| `Adafruit NeoPixel` | ≥1.15 |
| `ESP Async WebServer` | ≥3.11 |
| `AsyncTCP` | ≥1.1 |
| `ArduinoOTA` | (bundled with the ESP32 core) |

`AsyncTCP` 1.1.4 needs three patches on Arduino-ESP32 3.3.x, all lost on every
library upgrade — run `python3 scripts/patch_async_tcp.py` after installing it.

| Patch | Symptom if missing |
|---|---|
| 1 — `const status()` | Build fails at `ESPAsyncWebServer.h:1699` |
| 2 — `_tcp_new()` wrapper | Reboots on `assert failed: tcp_alloc` |
| 3 — callback registration | Reboots on `assert failed: tcp_arg` |

Patches 2 and 3 share one cause: Arduino-ESP32 3.x builds lwIP with
`CONFIG_LWIP_TCPIP_CORE_LOCKING=y` and `CONFIG_LWIP_CHECK_THREAD_SAFETY`, so any
lwIP call made from an ordinary FreeRTOS task aborts the board. Every call
AsyncTCP makes from an application task has to be marshalled onto the TCPIP
thread with `tcpip_api_call`. The script does this and then verifies the result:
`--check` fails loudly if any patch is missing or stale, and prints the count it
found. Run it after **every** library install or upgrade.

> The **Firebase Arduino Client Library is no longer used.** Removing it freed
> ~376 KB of flash, which is where the embedded dashboard lives.

## Build & verify

**Use `scripts/build.sh`.** It is the only supported build entry point:

```bash
./scripts/build.sh                    # compile to ./.build-out
./scripts/build.sh --clean            # full rebuild, no cache
```

It does three things, in this order:

1. regenerates `src/network/web_assets.h` from `frontend/`,
2. verifies the AsyncTCP patches are applied (and aborts with instructions if not),
3. compiles, with `--output-dir` so no stray 4 MB of binaries lands in `build/`.

Current size: **1,176,561 bytes (57%)** of the 2 MB app partition, 0 warnings.

Then the full gate suite:

```bash
./scripts/verify_all.sh --build
```

Runs every gate: the embedded-asset round-trip, the AsyncTCP patch check, the
admin-PIN unit tests, the dead-code rules, the documentation claims, an
end-to-end run of the real page against a mock board, the firmware build, and a
check that the dashboard really is in the resulting `.bin`. It builds through
`scripts/build.sh` too, so the gate never tests a build route nobody uses.

### Updating the dashboard

`frontend/` is the source of truth. The firmware does **not** read it at runtime
— it is compiled into flash by a generator, so change it and re-flash (or OTA)
the board. **There is nothing extra to run:** `scripts/build.sh` regenerates the
embedded copy on every build, so you cannot ship a firmware that serves a stale
page.

`src/network/web_assets.h` (~151 KB) is generated and **not committed** — it was
a fifth of the working tree and appeared five times in this repo's history for
~940 KB of blobs nobody reads. After a fresh clone, run `./scripts/build.sh`
once; `python3 scripts/embed_web.py` on its own also works.

`--check` verifies the generated header round-trips byte-for-byte, and
`--verify-binary <bin>` fails if the dashboard is not actually present in a
compiled firmware image.

## OTA Updates

1. Join the board's WiFi network.
2. Arduino IDE → Select the ESP32 network port (or enter `192.168.4.1`
   manually if it is not discovered).
3. Sketch → Upload.

The RGB LED turns blue during OTA and the dashboard shows progress.

## Architecture

Two FreeRTOS tasks on two cores:

- **Core 0 — `networkTask`** (prio 2): soft AP + captive DNS, the AsyncWebServer
  that serves the dashboard, the WebSocket, and ArduinoOTA.
- **Core 1 — `sensorTask`** (prio 2): ADC sampling, power math, limit checks,
  buzzer, LED, and the ~5 s energy save to NVS.

Shared `SystemData` is protected by a FreeRTOS mutex. `commit()` is not
thread-safe, so both cores serialise their NVS writes through it.

There is no third task: the old `firebaseTask` existed only to keep the cloud's
blocking TLS work away from the broadcast loop.

### The clock

The board has no internet, so it has no NTP. It **borrows the browser's clock**:
the dashboard sends its timestamp when the WebSocket opens and every 10 minutes,
and the firmware stores it in RTC memory so the time survives a restart with no
phone attached.

This matters beyond the clock display. `LimitManager::rolloverIfNeeded()`
deliberately does nothing while the clock is unset, so **without a connected
dashboard the monthly billing reset never happens.** Settings shows the clock
state for that reason.

### Notifications

Trip alerts are browser notifications, not a server push. There is no server.
They fire while a dashboard is open — which in AP-only mode means while you are
on the board's WiFi.

## Serial Monitor

- Baud: 115200
- The boot banner prints the network name, password and dashboard URL.
- `help` lists the commands:

  ```
  help / ?          status              debug
  ch <N>            cal                 info               wifi
  buzz <N>          inject <ch> <kwh>   reset <N>          reset_name [N]
  ---
  test led          rms_samples <N>     curr_cal <ch> <val>
  auto_zero <ch>    volt_cal <val>      clearwifi
  set_ap <name> <pw> reset_ap
  nvs_debug         reboot
  ```

  `status` and `debug` toggle live serial streams. `test led`, `nvs_debug`,
  `reboot`, `set_ap` and `reset_ap` are deferred: they are acknowledged
  immediately and run on Core 1, where blocking is safe.

`setwifi` no longer exists — the board has no network to join. `clearwifi` is
retained only to erase credentials stored by older firmware.

## Changing the network name and password

The board's own WiFi name and password are settings, not constants. Either from
**Settings → Access Point** in the dashboard, or over serial:

```
set_ap "Living Room Meter" hunter2hunter2
reset_ap
```

- Saved to flash immediately, then **the board reboots** to apply them. The
  reboot is deferred by a second so the confirmation is written out first.
- After the restart, your phone has dropped off — join the **new** network and
  reopen `http://192.168.4.1/`.
- Name: 1–32 characters, no leading/trailing spaces. Password: **8–63**
  characters. An open network (blank password) is not offered, because every
  mutating command on the board is behind the admin PIN and an open AP would
  hand that PIN to anyone in range.
- Nothing is written unless both fields are valid. A password the radio would
  refuse (under 8 characters) makes `softAP()` fail and would leave the board
  with no network at all, so it is rejected up front instead.
- **Forgot the password?** `reset_ap` (or the **Defaults** button) restores
  `ESP32-Elec-Counter` / `configure123` and reboots.
- The stored values are also what `wifi` prints, and the boot banner shows them
  — that banner is how you tell which network to join after a rename.

## Limitations

- **No access from outside your own WiFi.** This is the deliberate trade for
  removing the cloud. There is no port-forwarding and no remote access.
- **No true PWA install.** Service workers require a secure context, and
  `http://192.168.4.1` is not one, so the browser will offer "Add to Home
  Screen" (a shortcut) rather than a standalone install. Serving HTTPS from the
  board would need a self-signed certificate.
- **The AP radio never sleeps.** The old eco mode (modem sleep when nobody was
  watching) applied only to a station link and has no meaning for an access
  point, which must keep beaconing.
- **~10 clients** maximum, the ESP32 soft-AP limit.
- WiFi credentials from older firmware may still sit in NVS. Nothing reads them;
  run `clearwifi` to remove them.
# Lessons Learned

Hard-won knowledge that is **not** visible by reading the code. Architecture
itself lives in `doc/ARCHITECTURE.md`.

## Compilation / toolchain

1. **Never `.ino`-include a `src/` `.cpp`.** Arduino compiles every `.cpp`
   under the sketch root, so including one too gives multiple-definition link
   errors. `setup()`/`loop()` in the `.ino`, implementations in `src/`.
2. **Arduino's default build directory is `./build`** inside the sketch folder.
   That is a 4 MB artifact directory sitting next to your sources; it is
   gitignored and regenerated on every `arduino-cli compile`. Delete it freely
   when it holds a stale binary — but be aware a stale `.bin` there is a real
   footgun, because it is exactly the shape of "flash the wrong firmware".
3. **AsyncWebServer / AsyncWebSocket have no default constructors.** Hold them
   as pointers, allocate with `new`, initialise in `begin()`.
4. **Arduino's `DNSServer` method is `processNextRequest()`**, not
   `processNext()`.
5. **`beginResponse_P` is deprecated** in ESPAsyncWebServer 3.12. Use
   `beginResponse(code, mime, (const uint8_t*)data, len)`. The explicit-length
   `uint8_t*` overload matters: the `char*` overload measures with `strlen()`
   and would truncate every PNG at its first `0x00` byte.

## Library patches

- **AsyncTCP 1.1.4 on Arduino-ESP32 3.3.x needs two patches**, both lost on
  every library install/upgrade. Run `python3 scripts/patch_async_tcp.py`
  (idempotent) afterwards.
  1. `AsyncServer::status()` must be `const` — `ESPAsyncWebServer.h:1699` calls
     it through a const reference.
  2. `tcp_new_ip_type()` must be wrapped in `tcpip_api_call` (a `_tcp_new()`
     wrapper), or it hits `LWIP_ASSERT_CORE_LOCKED()` and reboots at
     `networkTask -> wsServer.startServer() -> server->begin()`.

## Measurement

- **Real power needs paired V-I samples.** Reading voltage then all six currents
  at each index gives the cross-product without any phase alignment.
- **Mean-remove the AC bias; never subtract a constant.** CT and ZMPT outputs
  are biased to mid-supply. A hard-coded `1.65f` would be wrong the moment the
  bias moves, which is why there is no `AC_BIAS_VOLTAGE` constant.
- **Do not EMA-filter the voltage reference.** It once shared channel 0's
  user-tunable LPF alpha. An EMA on a 50 Hz AC waveform attenuates its RMS
  amplitude: with `alpha = 0.01`, 230 V read as 147 V — and it only happened
  "when ANY calibration operation ran", because that is when the alpha changed.
  The current code mean-removes instead. **The general trap: a per-channel user
  control must never leak into a shared reference channel.**

## Concurrency

- **`Preferences::commit()` is `prefs.end()` + `prefs.begin()` and is NOT
  thread-safe.** With two cores both writing, it must be serialised. Every NVS
  write holds `dataMutex`, and `putFloat` alone does nothing without a
  following `commit()`.
- **`flushEnergy()` mattered more than it looked.** It sat for years without
  `dataMutex` because the only restart path (the STA connect-timeout reboot)
  was rare. Removing STA made `reboot` the *only* path and turned a latent race
  into a live one. **Removing a subsystem can make an existing latent bug
  load-bearing** — re-check anything that was previously "rare" and is now
  "always".

## Architecture decisions that were paid for

- **Limit checking must be standalone**, not embedded in a relay/pair state
  machine. When the relays were physically detached, the entire per-pair
  machine had to be ripped out. A plain 6-channel loop with a per-channel
  `tripNotified[]` latch survives hardware changes. Keep domain logic
  independent of actuation hardware.
- **Audible alerts belong in their own module** — `src/ui/buzzer.{h,cpp}`, a
  non-blocking millis-based driver driven from the sensor task. Never blocking
  `delay()`s in the sensing loop.
- **Self-hosting the dashboard was forced by the network, not chosen for
  elegance.** An `https://` page cannot open a `ws://` socket (mixed content),
  and the AP's captive DNS answers every hostname with the board's own IP. Both
  facts independently made a Firebase-hosted UI unable to show live data from a
  phone joined to the board's network.

## Process

- **The `.ino` must not include a `.cpp`, but the `.h` is fine** — headers have
  include guards and no linkage.
- **A tree listing in a doc makes dead files look alive.** `src/sensor/` and
  `src/utils/device_id.*` both sat in the documented tree long after they had
  no callers. A stale architecture diagram is worse than none, because it
  actively launders dead code as intentional.
- **Verify what you are documenting, against the code.** `scripts/setup.py`
  survived the removal of the entire cloud while cheerfully reporting `PASS
  frontend/config.js` for a file the project no longer had.
- **A gate you have only ever seen pass is not a gate.** Every check in
  `scripts/verify_all.sh` was deliberately broken at least once to confirm it
  fails. Two genuine bugs were found *because* an existing test had a negative
  control; one "failure" turned out to be an equivalent mutant and was resolved
  by diffing behaviour, not by adding a meaningless test.

## Archived — cloud era (2026-08)

Kept because the reasoning generalises; the code is gone.

- **A read that can fail must not share a connection with your hot path.** A
  per-loop RTDB poll on the same TLS session as the `/latest` push tore that
  session down every time the polled node was missing, forcing a fresh 1.3–1.9 s
  handshake. Dedicated connection, throttled, and treat `null` as normal.
- **Cloud OTA over RTDB `downloadOTA` was a dead end** on-device (TLS code
  −1000). Firmware delivery went back to USB/ArduinoOTA.
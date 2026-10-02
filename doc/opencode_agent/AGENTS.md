# AGENTS.md — ESP32-S3 Electricity Counter

**Read `doc/ARCHITECTURE.md` first.** It is the canonical, code-derived
reference for how this project works and why. This file deliberately does NOT
repeat it — a second copy of the architecture is a second copy to drift.

Everything below is what you cannot get from reading the code.

## What this project is, in one line

An ESP32-S3 that runs **only as a WiFi access point** and **serves its own
dashboard from flash** at `http://192.168.4.1/`. No station interface, no cloud,
no internet required.

## Traps — these will bite you

1. **Never add a station (STA) interface back.** There is no upstream network.
   If you need new network behaviour, it runs over the board's own AP.
2. **The board MUST serve the dashboard from flash.** The page is
   `http://192.168.4.1`, so `ws://192.168.4.1/ws` is same-origin. A browser
   refuses to open a `ws://` socket from an `https://` page (mixed content) —
   which is the entire reason the dashboard is self-hosted, and why an earlier
   read-only portal page had to exist.
3. **The board has no clock and no way to get one.** It borrows the browser's
   clock (`set_time`). `LimitManager::rolloverIfNeeded()` refuses to act while
   the clock is unset, so **the monthly billing reset silently never happens
   until a dashboard connects and lends it a time.** This is not cosmetic.
4. **Never `.ino`-include a `src/` `.cpp`.** Arduino compiles every `.cpp` under
   the sketch root; including one too gives multiple-definition link errors.
5. **`commit()` on `Preferences` is `prefs.end()` + `prefs.begin()` and is NOT
   thread-safe.** Every NVS write from both cores must hold `dataMutex`, and
   every one must actually `commit()` or it never reaches flash.
6. **Use AsyncTCP 3.x (ESP32Async) — never pin 1.1.4.** Arduino-ESP32 3.x builds
   lwIP with `CONFIG_LWIP_TCPIP_CORE_LOCKING=y` + `CONFIG_LWIP_CHECK_THREAD_SAFETY`,
   so any lwIP call from an ordinary FreeRTOS task aborts the board. 1.1.4 calls
   several of them and needed three patches (`scripts/patch_async_tcp.py`); 3.x
   marshals them upstream, so the patches are obsolete and cannot apply. The
   patcher detects the 3.x line and stands down. Install from
   `github.com/ESP32Async/*`, **not** the archived `me-no-dev` repo — the old
   web server also corrupted its `AsyncClient` under concurrent requests, which
   is why the dashboard could not load at all (see the root-cause section
   below). `build.sh` gates on this either way.
7. **Admin PIN is enforced on the ESP32, not in the UI.** See
   `src/network/auth_gate.cpp`. Hiding a button is convenience, not protection.
   New mutating verbs are gated automatically because the check runs *before*
   the verb dispatch — do not add verbs that bypass `processCommand()`.
8. **Mean-remove the AC bias; do not subtract a constant.** There is
   deliberately no `AC_BIAS_VOLTAGE`. Voltage and current are biased to
   mid-supply and the RMS math removes that per sample.
9. **Do not EMA-filter the voltage reference.** It did once share channel 0's
   user-tunable LPF alpha, and a 50 Hz AC waveform through a slow EMA reads
   ~147 V instead of ~240 V. See `lessons.md`.
10. **The WS2812 R/G outputs are physically swapped** and compensated in
    software. There is a comment saying so. Do not "fix" it.

## Before you change anything

```bash
./scripts/build.sh                   # THE build command — nothing else
./scripts/verify_all.sh --build      # every gate, ~2 min
```

`verify_all.sh` checks the embedded dashboard assets byte-for-byte, verifies the
AsyncTCP patches, unit-tests the PIN gate, scans the frontend for dead cloud
code, enforces the dead-code rules, verifies the documentation's claims, runs the
real page against a mock board, and compiles with `-Werror`-style strictness.

**If you edit `frontend/`, do not regenerate anything yourself.**
`src/network/web_assets.h` is generated, is *not* committed, and `build.sh`
rebuilds it on every compile — the firmware can never serve a stale page. If you
find yourself hand-running `embed_web.py` to "catch up", something has gone
wrong with the build path; fix that instead.

## Hardware facts

| Signal | Pins |
|---|---|
| CT current ch1–6 | GPIO7, 5, 6, 8, 4, 2 |
| Voltage reference | GPIO1 |
| Active buzzer | GPIO13 (N beeps = channel number) |
| RGB LED | GPIO48 (WS2812, R/G swapped) |

AP network: `ESP32-Elec-Counter` / `configure123`, board IP `192.168.4.1`.
Admin PIN default: `1234` (change it on first boot).

## Never trust a checker you have only ever seen pass

Every gate here was proven able to fail — assets against a real pre-migration
binary, the PIN gate by mutation, the E2E suite against four broken copies of
`script.js`. If you add an assertion, break the thing it claims to protect and
confirm it goes red. Three real bugs in this codebase were only found because
an existing test had a negative control.
## AsyncTCP Patches (2026-10-02)

All three are lost on every library install/upgrade — run
`python3 scripts/patch_async_tcp.py` afterwards. It is idempotent and
self-verifying; `--check` exits non-zero if anything is missing or stale.

| Patch | Symptom if missing |
|---|---|
| 1 — `const status()` | Build fails at `ESPAsyncWebServer.h:1699` |
| 2 — `_tcp_new()` wrapper | Reboots on `assert failed: tcp_alloc` |
| 3 — callback registration | Reboots on `assert failed: tcp_arg` |

Patches 2 and 3 share one cause: Arduino-ESP32 3.x builds lwIP with
`CONFIG_LWIP_TCPIP_CORE_LOCKING=y` + `CONFIG_LWIP_CHECK_THREAD_SAFETY`, so any
lwIP call made from an ordinary FreeRTOS task aborts the board. Patch 3
marshals callback registration (`tcp_arg`/`tcp_recv`/`tcp_sent`/`tcp_err`/
`tcp_poll`/`tcp_accept`) for the five sites that run on an application task. It
deliberately leaves `AsyncClient::AsyncClient(tcp_pcb*)`, `_error()` and
`_lwip_fin()` raw: those already run on the TCPIP thread, and marshalling them
would deadlock instead of fixing anything.

Fixing one core-locked call proved nothing on its own — the board simply aborted
on the next one. Enumerate every core-locked call in the file and decide per
site which thread it runs on, rather than patching the one that happened to
crash first.

## The dashboard cannot load — root cause (2026-10-02)

**The board serves exactly one TCP connection at a time. Two concurrent HTTP
requests kill it.** A browser opens six, so the dashboard has never been able to
load. This is an upstream library defect, not a frontend or firmware bug.

```
Guru Meditation Error: Core 0 panic'ed (LoadProhibited). Exception was unhandled.
EXCVADDR: 0x00000000
PC 0x42016894 -> AsyncClient::onData                              AsyncTCP.cpp:744
                AsyncWebServerRequest::AsyncWebServerRequest      WebRequest.cpp:77
```

`std_function.h:391` executes `__x._M_manager(...)`, so `EXCVADDR 0x0` means the
`AsyncClient` was already corrupt when its callbacks were installed — the three
`onXxx()` calls before it in the constructor succeeded, so the pointer was valid
and the object itself was clobbered. Heap corruption, not exhaustion: 128 KB was
free at the moment of the crash.

### Measured, not assumed

| Probe | Result |
|---|---|
| 1 request at a time | every asset `200`, byte-exact |
| 2 concurrent tiny assets (951/1221/1116 B) | **panics** |
| 3 concurrent, all 6 page assets | **panics** in <1 s |
| Free heap at crash | 128 KB — not exhaustion |
| Embedded CSS vs `frontend/style.css` | byte-identical |
| `?v=` routing, service worker, cache | all ruled out |

It is **not** the AsyncTCP patches: the crash is in the *unpatched*
`AsyncWebServerRequest` constructor path, which runs before any patched code,
and `AsyncClient::AsyncClient(tcp_pcb*)` is deliberately left raw.

### Why the symptoms looked like a CSS problem

Chrome reports the crash as `ERR_CONNECTION_RESET 200 (OK)` — the status line
arrives, then Core 0 dies mid-body — followed by `ERR_INTERNET_DISCONNECTED` for
every request queued behind it. `index.html` (requested first) renders, then
`style.css` and `script.js` never arrive, so the page is unstyled and
`showPage is not defined`. The dashboard you see may be a **cached** copy.

### Reproduce it (do not debug this by eye)

Join the AP, then hammer it. Never test one request at a time — that always
passes and hides the bug.

```bash
for r in $(seq 1 8); do
  for a in manifest.json icons/icon-192.png; do
    curl -sS -o /dev/null --max-time 4 "http://192.168.4.1/$a" &
  done; wait
done
```

Two parallel `curl`s are enough. Watch the serial console for `Guru Meditation`.
Opening `/dev/ttyACM*` asserts DTR/RTS and resets the board — deassert it first
and hold one fd open, never reopen per read.

### Fix — done 2026-10-02

Swapped to the maintained successor: **`ESP32Async/AsyncTCP` 3.5.0** (21 native
core-lock call sites) + `ESP32Async/ESPAsyncWebServer`. Compiles with zero
warnings, needs none of the three patches, and survives the 6-concurrent stress
that used to kill it in under a second. Old libraries kept at
`/tmp/opencode/libbackup/` for diffing.

`scripts/patch_async_tcp.py` now reads `library.properties` and stands down on
3.x instead of failing every build for anchors that no longer exist. Verified in
all three directions: 3.5.0 passes, a reverted 1.1.4 fails, a patched 1.1.4
passes.

STA mode is **not** a workaround — the crash is above the radio and reproduces
identically in AP mode, which also contradicts trap #1 anyway.

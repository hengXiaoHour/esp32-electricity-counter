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
6. **AsyncTCP 1.1.4 needs patching on Arduino-ESP32 3.3.x.** Run
   `python3 scripts/patch_async_tcp.py` after any library install or upgrade.
   It is idempotent. Skipping it fails the build at `ESPAsyncWebServer.h:1699`
   for patch #1 — but patch #2 **does not fail the build at all**. Without it
   the firmware compiles cleanly and then reboot-loops on the board with
   `LWIP_ASSERT_CORE_LOCKED` from `server->begin()`. `build.sh` checks both
   before compiling, for that reason.
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

# Lessons Learned

## Compilation Lessons
1. **Arduino .ino must not include src/.cpp files** — Arduino build system auto-compiles all .cpp in src/, causing multiple definition errors if the .ino also #includes them. Keep setup/loop in .ino, module implementations in src/.
2. **DNSServer API** — ESP32 core's DNSServer uses `processNextRequest()`, not `processNext()`.
3. **AsyncWebServer/AsyncWebSocket no default constructors** — Must use pointers (new) and initialize in begin().

## Library Patches
- AsyncTCP 1.1.4: `status()` must be `const` for compatibility with ESP Async WebServer 3.11.2. Patched both .h and .cpp.

## Architecture Lessons
- Power calculation needs paired V-I samples for real power. Combined sampling (voltage + all current channels per index) gives best accuracy without complex phase alignment.
- WS2812 R/G swap must be clearly documented with a comment warning not to "fix" it.
- **Limit checking must be standalone, not embedded in a relay/changeover-pair state machine** (v1.2.0 refactor). When relays were physically detached, the whole per-pair machine (PairState, manual overrides, relayOn bookkeeping) had to be ripped out. A pure 6-channel `checkLimits()` loop over `NUM_CHANNELS` with a per-channel `tripNotified[]` flag is simpler and survives hardware changes. Keep domain logic (limits, alerts) independent of any actuation hardware.
- **Audible alerts belong in their own module**: the active buzzer lives in `src/ui/buzzer.{h,cpp}` as a non-blocking, millis-based beep driver (`ring(beeps)` + `loop()`), driven from the sensor task — never blocking `delay()`s in the sensing loop.

## Firebase Sync Latency — Regression & Fix (2026-08-08)

### Symptom
Cloud dashboard sync slowed/stalled on live boards.

### Root cause
A per-loop RTDB read that could fail (missing node) was running on the same TLS connection as `/latest` pushes. A missing node returned an error and tore down the shared TLS session → fresh ~1.3-1.9 s handshake on every push.

### Fix
- Any RTDB read that can fail (missing node) must use a **dedicated `FirebaseData` connection** — never share the publish connection.
- Throttle infrequent polls, don't run them every loop iteration.
- Treat a `null`/empty node as **normal** (no teardown) — mirror the `pollCommands()` pattern.

### Lesson
Any per-loop RTDB read that can fail (missing node) must go on a dedicated connection and be throttled, or it silently destroys the push path.

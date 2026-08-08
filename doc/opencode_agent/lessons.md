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

## Cloud OTA via Firebase — Research Notes (2026-08-08, unresolved)

### Problem
Deliver firmware updates through Firebase (Cloud RTDB + Firebase Hosting) so the device can be flashed from anywhere without LAN access.

### What works
- `tools/ota_upload.py` compiles, publishes .bin to Firebase Hosting, writes RTDB `/ota {version, url, md5}`, polls status, confirms boot — **fully working pipeline**
- Device downloads via raw HTTPS (`WiFiClientSecure` + `HTTPClient`) — reaches 95-100% with correct MD5
- `stopWiFiClient()` frees ~150KB heap by releasing Firebase-ESP-Client's SSL socket — `Update.begin()` succeeds after this

### What doesn't work
- **Task watchdog (WDT) resets the chip** during the ~60s download. The IDLE task on Core 0 starves because the firebaseTask (priority 1, Core 0) blocks continuously. The Arduino core registers IDLE0 with the WDT (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y`), timeout 5s.
- `delay(1)` in the download loop should yield to IDLE, but the WDT still fires — possibly because `Update.write()` blocks interrupts long enough, or the yield isn't reaching IDLE in time.
- `disableCore0WDT()` fails with "task not found" (Arduino bug in the status check).
- `esp_task_wdt_init()` fails with "TWDT already initialized" (Arduino already initialized it).

### Approaches attempted (all failed)
1. `delay(1)` every chunk — WDT still fires
2. `taskYIELD()` every 64KB — only yields to higher-priority tasks, not IDLE
3. `disableCore0WDT()` — "task not found"
4. `esp_task_wdt_init(60)` — "TWDT already initialized"
5. `esp_task_wdt_delete(NULL)` — "task not found" (firebaseTask not WDT-registered)

### Promising untested approach
- **Move download to `loop()` (Core 1)**: `loopTask` runs on Core 1 (`ARDUINO_RUNNING_CORE=1`) and is NOT registered with the WDT by default. The IDLE task on Core 1 is also NOT monitored (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1` is not set). So a blocking download in `loop()` should not trigger WDT. The firebaseTask on Core 0 just polls for triggers and sets a flag.
- Code was written (`src/network/cloud_ota.{h,cpp}`) and compiled but not successfully flashed/tested.

### Key technical facts
- Arduino `loopTask` is NOT WDT-enabled by default (`loopTaskWDTEnabled` starts false)
- `loopTask` runs on Core 1 by default
- Only IDLE0 is WDT-monitored (not IDLE1)
- Firebase-ESP-Client's `stopWiFiClient()` releases SSL socket + ~16KB buffers
- `UPDATE_SIZE_UNKNOWN` works for `Update.begin()` (avoids needing exact size)
- ESP-IDF docs recommend `bulk_flash_erase` or increasing WDT timeout for large OTA

### References
- ESP-IDF OTA docs: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ota.html
- FirebaseClient OTA discussion: https://github.com/mobizt/FirebaseClient/discussions/259
- WDT + OTA issue: https://github.com/espressif/arduino-esp32/issues/3959
- FirebaseClient has built-in `downloadOTA()` in newer versions (4.4.17 has it) — might handle WDT internally

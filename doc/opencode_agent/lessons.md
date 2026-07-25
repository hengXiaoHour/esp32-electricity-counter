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

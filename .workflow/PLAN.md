# Plan: Switch Cloud OTA to Firebase-ESP-Client built-in RTDB downloadOTA

## Context
Hand-rolled `WiFiClientSecure + HTTPClient` download from Firebase Hosting keeps
failing (ENOMEM / TLS hang with ~96 KB free heap). The installed library
(v4.4.17) has a proven, chunked, WDT-safe `Firebase.RTDB.downloadOTA()` that
streams a base64 firmware blob from RTDB over the existing fbdo TLS session.
Storage bucket is NOT enabled in this project, so RTDB (base64) is the delivery path.

## Files to Modify

- `src/network/cloud_ota.{h,cpp}` — rewrite download path to
  `Firebase.RTDB.downloadOTA`; keep state/progress/md5 API surface.
- `src/network/firebase_bridge.cpp` — `handleOtaRequest()` must NOT call
  `stopWiFiClient()`; trigger the new blocking download instead.
- `tools/ota_upload.py` — write base64 firmware to RTDB `/devices/<id>/firmware` with
  the `file,base64,` signature; keep the `/devices/<id>/ota` trigger; drop Hosting publish.
- `database.rules.json` — allow read of `firmware` node (public) so device can read;
  writes stay `auth != null` (service account / script).
- Possibly `.ino` — OTA driver comment stays valid (loop() drives nothing now);
  verify firebaseTask handles the blocking call.

## Implementation Steps
1. `cloud_ota.{h,cpp}`:
   - Add `#include <Firebase_ESP_Client.h>`, keep `Update.h` (library uses it).
   - Add `bool FirebaseBridge`-owned `downloadRTDB(FirebaseData &fbdo,
     const String &fwPath, const String &md5)` that calls
     `Firebase.RTDB.downloadOTA(&fb, fwPath, cb)` blocking with a static callback.
   - Callback maps status → progress/state strings (keep `getState()`/`getProgress()`
     working for WebSocket/WS dashboard + web console); on complete store appliedMd5.
   - Remove HTTPClient / WiFiClientSecure idling code (keep Update for library? library
     drives Update; we don't call it—remove our Update.use too).
   - Keep `trigger(version,url,md5)` for internal state.
   - Verification: compile.
2. `firebase_bridge.cpp`:
   - In `handleOtaRequest()`: build fw path `/devices/<id>/firmware`, call
     `cloudOta.startRefresh...` → actually call `cloudOta.download_rtdb(fbdo,
     rtdbPath("firmware"), md5)` directly (blocking in firebaseTask). Remove
     `fbdo.stopWiFiClient()`/`fbCmd.stopWiFiClient()`.
   - Verification: compile; smoke on serial.
3. `tools/ota_upload.py`:
   - Add `publish_firmware_to_rtdb(db, device, bin, token)`: read bin bytes, base64,
     compute pad sig (`file/File/fIle`), PUT `dev_path(device,"firmware.json")` with
     JSON string `'"<sig>"...'`; assert HTTP 200.
   - Keep writing `/devices/<id>/ota` `{version,url,md5,ts}` trigger.
   - Remove `publish_to_hosting` call path (or stub) — keep URL field optional.
   - Verification: `python3 tools/ota_upload.py --device esp-a172e0`.
4. `database.rules.json`:
   - Add `"firmware": { ".read": true, ".write": "auth != null" }`.
   - Verify deploy via PUT `.settings/rules.json`.
5. Build & flash to `esp-a172e0`, then TEST: publish 2.4.21, polka boot; check serial.

## Test Strategy
- Unit: none (embedded). Step-by-step compile then live flash.
- Live: upload via `ota_upload.py --device esp-a172e0`, watch `/devices/esp-a172e0/ota`,
  confirm device rebooted to new FW (`/devices/esp-a172e0/latest`).

## Rollback
- Keep Hosting publish (just don't delete). If RTDB OTA fails, revert to Hosting path
  (git checkout cloud_ota.*).
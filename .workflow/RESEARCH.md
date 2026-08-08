# Research: Switch Cloud OTA to Firebase-ESP-Client's built-in RTDB downloadOTA

## Current State

`cloud_ota.{h,cpp}` hand-rolls the download with `WiFiClientSecure + HTTPClient`:
- fetches firmware from Firebase Hosting (`https://esp32-electricity-counter.web.app/firmware/<ver>.bin`)
- `sendRequest("GET")` + `getStreamPtr()` chunked read → `Update.write`
- md5 verified via `MD5Builder`, `appliedMd5` kept in NVS `ota/appliedMd5`

**Why it keeps failing:** `HTTPClient::GET()` buffers the whole 1.4 MB body into RAM
(~96 KB free heap) → ENOMEM / TLS hang. The `sendRequest`+stream workaround still
relies on a **second** TLS stack (BearSSL via `WiFiClientSecure`) that the Firebase
library already manages internally.

## Key Finding: the library already does this

Firebase-ESP-Client v4.4.17 ships a **built-in, streaming, WDT-safe RTDB OTA** path:

```
bool Firebase.RTDB.downloadOTA(FirebaseData *fbdo, <path>, RTDB_DownloadProgressCallback cb = NULL)
```
(`src/Firebase.h:1910`, `src/rtdb/FB_RTDB.h:2082`, `examples/RTDB/DownloadFileOTA/`)

- Firmware lives in RTDB as a base64 **string** at a node path (written by the
  library's `setFile`/`pushFile`). Device GETs it chunk-by-chunk on the SAME fbdo
  TLS session.
- `prepareDownloadOTA()` → `Update.begin(size)`; chunks are base64-decoded via
  `decodeBase64OTA()` and written by `Update.write`; `endDownloadOTA()` → `Update.end()`.
- All guarded by `OTA_UPDATE_ENABLED`, which is auto-defined when
  `ENABLE_OTA_FIRMWARE_UPDATE`/`FIREBASE_ENABLE_OTA_FIRMWARE_UPDATE` is set AND
  RTDB/Storage is enabled (`src/FB_Const.h:53-59`).
- `RTDB_DownloadStatusInfo` callback: `status` (init/download/complete/error),
  `progress`, `size`, `elapsedTime`, `errorMsg`.

### RTDB firmware blob format (what the uploader must write)

`setFile` stores: JSON string `"file,base64,<b64data>"` with a **pad-length signature**:
- `n%3==0` → `"file,base64,`
- `n%3==2` → `"File,base64,`  (s[1]='F')
- `n%3==1` → `"fIle,base64,`  (s[2]='I')

The device decoder (`FB_Session.cpp:1202`, `payloadOfs=13`) strips the leading
`"file,base64,` and the trailing quote, so the uploader must produce a JSON string
literal that starts with that signature and ends with a closing `"`.

### RX buffer

Per the example comment, the library bumps the fbdo RX buffer to 16 kB *during* the
OTA download and restores the configured size after — so no `stopWiFiClient()` hack is
needed. Our `setBSSLBufferSize(4096, 4096)` is fine.

## Decision

Replace the hand-rolled HTTPS download with `Firebase.RTDB.downloadOTA()`:

1. **Uploader** (`tools/ota_upload.py`): PUT the base64 firmware to
   `/devices/<id>/firmware` with the `file,base64,` signature (exact `setFile` format),
   then write the `/devices/<id>/ota` trigger `{version, md5, ts}` (url kept for logs).
   → delete Hosting publish of the bin.
2. **Device** (`cloud_ota.cpp`): `downloadRTDB(fbdo, path, md5)` wraps
   `Firebase.RTDB.downloadOTA` with a static callback; on complete stores `appliedMd5`
   and reboots. Runs in firebaseTask (Core 0, 32k stack, not WDT-monitored); the
   sensor task (Core 1) and broadcast task (Core 0, prio 2) keep running.
3. **firebase_bridge**: `handleOtaRequest()` stops calling `stopWiFiClient()` (would
   kill the fbdo TLS session `downloadOTA` needs) and calls the new blocking download.

## Risks
- RTDB blob is ~2 MB base64 string — single-node value size is fine (REST PUT);
  device reads it in chunks.
- md5 is no longer verified byte-for-byte by the device (library does its own
  `Update.end()` integrity + bootloader check on reboot). `appliedMd5` stored from the
  trigger so re-triggers are still deduped.
- `downloadOTA` is blocking → firebaseTask idle loop after it is dead code (device
  reboots first). Harmless.

## Open Questions
- Keep or drop Hosting publishing? → **Drop** (OTA no longer needs it; keeps the
  release lean). Frontend still served separately.

#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "../config.h"
#include "cloud_cfg.h"

class NVSManager;

// Remote monitoring: pushes a small snapshot to a realtime database over
// plain HTTPS REST (PATCH /devices/<MAC>/latest.json?auth=...), every
// PUSH_INTERVAL_MS, STA-only.
//
// The deliberate differences from the removed cloud era
// (doc/opencode_agent/lessons.md, "Archived - cloud era").
//
//  1. No SDK. The old Firebase Arduino client owned a blocking TLS handshake
//     on the hot path. This is ~60 lines around WiFiClientSecure and nothing
//     else, so there is no library to rot, patch, or fill flash (classic
//     ESP32 is already at 95%).
//  2. Push-only. The old per-loop RTDB poll shared the push's TLS session and
//     tore it down whenever the polled node was missing (1.3-1.9 s handshake
//     each time). This NEVER reads: it sends PATCH, glances at the status
//     line for " 200 ", and closes. A fresh connection per push, so there is
//     no session to go stale.
//  3. STA-only. stationUp() gates every attempt; on the fallback AP (no
//     internet by definition) loop() returns before touching TLS.
//  4. MAC identity. The device path comes from WiFi.macAddress(), formatted by
//     cloud_formatDeviceId(). There is no settable board id and no NVS key
//     for one - nothing to mistype, nothing to orphan.
//  5. The token NEVER leaves the board except inside the TLS tunnel to the
//     database host. It is in no snapshot, no log line, no dashboard field.
//
// Cost, stated honestly: the handshake + PATCH blocks the NETWORK task for
// typically under 2 s per push (timeout 4 s). Broadcasts pause during that
// window; sensing on Core 1 never notices. Enabling cloud also keeps eco from
// sleeping (see wantsRadio) - a radio that naps cannot push.
class CloudPush {
public:
  void begin(NVSManager *nvsRef);
  // Call from networkTask (~20 ms tick). Takes dataMutex briefly to snapshot
  // the readings, then does TLS with the mutex RELEASED - a multi-second
  // handshake must never hold the lock the sensor task needs.
  void loop(SystemData *sysData, SemaphoreHandle_t *mutex);

  bool enabled() const { return enabled_; }
  // True while cloud needs the radio awake. updateEcoMode() counts this as
  // watched: cloud on = eco off, stated in the UI so it is not mysterious.
  bool wantsRadio() const;
  bool lastPushOk() const { return lastOk_; }
  // Seconds since the last 200 OK, or UINT32_MAX when nothing ever landed.
  uint32_t secondsSincePush() const;
  // 12-char MAC id, or "" when the radio has no MAC to read.
  const char *deviceId() const { return deviceId_; }

  static const uint32_t PUSH_INTERVAL_MS = 10000;

private:
  NVSManager *nvs = nullptr;
  bool enabled_ = false;
  char host_[CLOUD_MAX_HOST_LEN + 1] = {0};
  char auth_[CLOUD_MAX_AUTH_LEN + 1] = {0};
  char deviceId_[CLOUD_DEVICE_ID_LEN + 1] = {0};
  bool lastOk_ = false;
  uint32_t lastAttemptMs_ = 0;
  uint32_t lastOkMs_ = 0;

  bool snapshot(SystemData *sysData, SemaphoreHandle_t *mutex, String &body);
  bool post(const String &body);
};

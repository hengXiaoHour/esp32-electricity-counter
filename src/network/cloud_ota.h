#pragma once

#include <Arduino.h>
#include <Firebase_ESP_Client.h>

#include "../config.h"

// Cloud OTA via Firebase RTDB, using the Firebase-ESP-Client library's
// built-in streaming downloadOTA. The firmware blob is stored as a base64
// string at /devices/<id>/firmware (written by tools/ota_upload.py) and the
// device downloads it over the SAME fbdo TLS session that already works for
// /latest pushes — no second BearSSL stack, no RAM-hogging body buffer.
class CloudOTA {
public:
  CloudOTA();

  void begin();

  // Called from loop() (no-op; kept for interface compatibility).
  void loop();

  // Blocking RTDB OTA download. Runs from firebaseTask (Core 0, 32k stack, not
  // WDT-monitored). On success stores appliedMd5 and reboots; on failure
  // returns false (device keeps running, bridge stays paused until next boot).
  bool download(FirebaseData &fb, const String &fwPath, const String &md5);

bool isInProgress() const { return inProgress; }
  uint8_t getProgress() const { return progress; }
  const String &getState() const { return state; }
  const String &getAppliedMd5() const { return appliedMd5; }

  // Threaded to the Firebase download callback.
  void onStatus(const RTDB_DownloadStatusInfo &info);

private:
  bool inProgress;
  uint8_t progress;
  String state;
  String appliedMd5;
};

extern CloudOTA cloudOta;
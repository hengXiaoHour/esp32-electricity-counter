#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include "../config.h"

class CloudOTA {
public:
  CloudOTA();

  void begin();

  // Called every loop(). Runs the firmware download in the loopTask context
  // (Core 1), which is NOT registered with the task watchdog — this avoids
  // the WDT reset that happens when a high-priority task blocks for minutes.
  void loop();

  // Trigger an OTA download. Called by the firebaseTask when a new firmware
  // version is detected. Non-blocking: the actual download runs in loop().
  void trigger(const String &version, const String &url, const String &md5);

  bool isInProgress() const { return inProgress; }
  uint8_t getProgress() const { return progress; }
  const String &getState() const { return state; }
  const String &getAppliedMd5() const { return appliedMd5; }

private:
  bool inProgress;
  uint8_t progress;
  String state;
  String pendingVersion;
  String pendingUrl;
  String pendingMd5;
  bool hasPending;
  String appliedMd5;

  void runUpdate();
};

extern CloudOTA cloudOta;


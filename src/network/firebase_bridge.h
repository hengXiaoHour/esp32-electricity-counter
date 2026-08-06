#pragma once

#include <Firebase_ESP_Client.h>
#include "../config.h"
#include "../core/power_calculator.h"

class LimitManager;
class NVSManager;
class SystemData;

// Publishes the live system snapshot to Firebase Realtime Database (`/latest`)
// every FIREBASE_PUSH_INTERVAL_MS and polls the `/commands` node for queued
// dashboard commands. Uses service-account JWT auth (see firebase_config.h).
//
// Runs on Core 0 (network task) alongside the WebSocket server. If Firebase
// credentials are empty/not configured, the bridge is inert (no-op).
class FirebaseBridge {
public:
  FirebaseBridge();

  void begin(NVSManager &nvsRef,
             SystemData *sysDataRef, SemaphoreHandle_t *mutexRef,
             PowerCalculator *powerCalcRef, LimitManager *limitMgrRef);

  // Configure + start auth using firebase_config.h credentials.
  // Call once after WiFi connects.
  void start();

  // Returns true when credentials are present and auth succeeded.
  bool ready() const;

  // Called every loop from networkTask. Maintains auth, polls commands.
  void loop();

  // Throttled to FIREBASE_PUSH_INTERVAL_MS. Serializes the live snapshot and
  // writes it to `/latest`.
  void pushLatest();

  bool configured() const;

private:
  NVSManager *nvs;
  SystemData *sysData;
  SemaphoreHandle_t *dataMutex;
  PowerCalculator *powerCalc;
  LimitManager *limitMgr;

  FirebaseData fbdo;
  FirebaseAuth auth;
  FirebaseConfig config;

  bool started;
  uint32_t lastPush;
  uint32_t lastCommandPoll;

  void buildLatestJson(FirebaseJson &json);
  void pollCommands();
};

// Shared JSON builder used by both WebSocketServer and FirebaseBridge
// (firmwareVersion, epoch, lastMonth, ntfy, ch[], events[]).
void buildSystemJson(const SystemData &data, PowerCalculator *powerCalc,
                     NVSManager *nvs, String &json);
#pragma once

#include <Firebase_ESP_Client.h>
#include "../config.h"
#include "../core/power_calculator.h"

// How often the snapshot is pushed to `/latest` and commands are polled.
// A TLS round-trip takes ~150-300 ms on WiFi, so 1 s cadence is snappy for
// the cloud dashboard while barely touching the WebSocket broadcast loop
// (which keeps its own 150 ms cadence on the shared radio).
#define FIREBASE_PUSH_INTERVAL_MS 1000
#define FIREBASE_COMMAND_POLL_MS 1000

class LimitManager;
class NVSManager;
class SystemData;
class CloudOTA;

// Publishes the live system snapshot to Firebase Realtime Database (`/latest`)
// every FIREBASE_PUSH_INTERVAL_MS and polls the `/commands` node for queued
// dashboard commands. Uses service-account JWT auth (see firebase_config.h).
//
// Runs on its OWN Core 0 task (firebaseTask), NOT inside networkTask: the
// synchronous HTTPS calls (setJSON/getJSON) would block the WebSocket broadcast
// loop. If Firebase credentials are empty/not configured, the bridge is inert.
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

  // Check RTDB /ota node for a new firmware version. If found (and not
  // already applied), requests an OTA (does not start immediately — the
  // caller must call handleOtaRequest() to free SSL heap first).
  bool checkOtaTrigger();

  // Returns true if an OTA was requested via checkOtaTrigger() but not yet
  // handled. The firebaseTask calls this, stops Firebase I/O, frees the
  // SSL socket (~150KB heap), then lets CloudOTA begin the download.
  bool otaRequested() const { return otaReq; }

  // Frees the Firebase-ESP-Client SSL buffers and triggers the CloudOTA
  // download. Must be called from firebaseTask (Core 0) BEFORE CloudOTA
  // starts its HTTPS transfer. After this, pause Firebase until reboot.
  void handleOtaRequest();

private:
  NVSManager *nvs;
  SystemData *sysData;
  SemaphoreHandle_t *dataMutex;
  PowerCalculator *powerCalc;
  LimitManager *limitMgr;

  FirebaseData fbdo;     // /latest push (own persistent TLS connection)
  FirebaseData fbCmd;    // /commands poll (separate, keeps its own connection)
  FirebaseAuth auth;
  FirebaseConfig config;

  bool started;
  bool paused;       // true after OTA is triggered (Firebase I/O suspended)
  uint32_t lastPush;
  uint32_t lastCommandPoll;

  bool otaReq;       // set by checkOtaTrigger(), cleared by handleOtaRequest()
  String pendingOtaVersion;
  String pendingOtaUrl;
  String pendingOtaMd5;

  void buildLatestJson(FirebaseJson &json);
  void pollCommands();
};

// Shared JSON builder used by both WebSocketServer and FirebaseBridge
// (firmwareVersion, epoch, lastMonth, ntfy, ch[], events[]).
void buildSystemJson(const SystemData &data, PowerCalculator *powerCalc,
                     NVSManager *nvs, String &json);
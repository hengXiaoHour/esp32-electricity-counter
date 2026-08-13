#pragma once

#include <Firebase_ESP_Client.h>
#include "../config.h"
#include "../core/power_calculator.h"

#define FIREBASE_PUSH_INTERVAL_MS 1000
#define FIREBASE_COMMAND_POLL_MS 1000

class LimitManager;
class NVSManager;
class SystemData;

class FirebaseBridge {
public:
  FirebaseBridge();

  void begin(NVSManager &nvsRef,
             SystemData *sysDataRef, SemaphoreHandle_t *mutexRef,
             PowerCalculator *powerCalcRef, LimitManager *limitMgrRef);

  void start();
  bool ready() const;
  void loop();
  void pushLatest();
  bool configured() const;

private:
  NVSManager *nvs;
  SystemData *sysData;
  SemaphoreHandle_t *dataMutex;
  PowerCalculator *powerCalc;
  LimitManager *limitMgr;

  FirebaseData fbdo;
  FirebaseData fbCmd;
  FirebaseAuth auth;
  FirebaseConfig config;

  bool started;
  uint32_t lastPush;
  uint32_t lastCommandPoll;
  String consoleErr;

  void buildLatestJson(FirebaseJson &json);
  void pollCommands();
};

void buildSystemJson(const SystemData &data, PowerCalculator *powerCalc,
                     NVSManager *nvs, String &json);

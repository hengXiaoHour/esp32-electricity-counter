#include "firebase_bridge.h"
#include "firebase_config.h"
#include "command_processor.h"
#include "console_handler.h"
#include "../core/limit_manager.h"
#include "../utils/nvs_manager.h"
#include "../utils/device_id.h"

#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <time.h>

static String rtdbPath(const char *node) {
  return String("/devices/") + deviceId() + "/" + node;
}

void buildSystemJson(const SystemData &data, PowerCalculator *powerCalc,
                     NVSManager *nvs, String &json) {
  json = "{\"v\":";
  json += String(data.voltageRMS, 1);
  json += ",\"uptime\":";
  json += data.uptime;
  json += ",\"wifi\":";
  json += data.wifiConnected ? "true" : "false";
  json += ",\"rssi\":";
  json += data.wifiRSSI;
  json += ",\"ap\":";
  json += data.apMode ? "true" : "false";
  json += ",\"voltageCalibration\":";
  json += String(data.voltageCalibration, 1);
  json += ",\"currentCalibration\":[";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    json += String(data.currentCalibration[i], 1);
    if (i < NUM_CHANNELS - 1) json += ",";
  }
  json += "]";
  json += ",\"rmsSamples\":";
  json += data.rmsSamples;
  json += ",\"noiseFloor\":[";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    json += String(powerCalc->noiseFloor[i], 3);
    if (i < NUM_CHANNELS - 1) json += ",";
  }
  json += "]";
  json += ",\"firmwareVersion\":\"";
  json += FIRMWARE_VERSION;
  json += "\",\"epoch\":";
  json += (long)time(nullptr);
  json += ",\"lastMonth\":";
  json += nvs->loadLastMonth();
  json += ",\"ntfy\":{\"topic\":\"";
  json += nvs->loadNtfyTopic();
  json += "\",\"enabled\":";
  json += nvs->loadNtfyEnabled() ? "true" : "false";
  json += "},\"ch\":[";

  for (int i = 0; i < NUM_CHANNELS; i++) {
    const ChannelData &ch = data.channels[i];
    json += "{\"n\":\"";
    json += ch.name;
    json += "\",\"a\":";
    json += String(ch.currentRMS, 2);
    json += ",\"w\":";
    json += String(ch.activePower, 1);
    json += ",\"va\":";
    json += String(ch.apparentPower, 1);
    json += ",\"pf\":";
    json += String(ch.powerFactor, 3);
    json += ",\"kwh\":";
    json += String(ch.energyKWh, 3);
    json += ",\"s\":";
    json += ch.status;
    json += ",\"mkwh\":";
    json += String(ch.monthlyKwhLimit, 1);
    json += "}";
    if (i < NUM_CHANNELS - 1) json += ",";
  }

  json += "],\"events\":[";

  int start = data.eventCount > 10 ? data.eventCount - 10 : 0;
  for (int i = start; i < data.eventCount; i++) {
    const Event &ev = data.events[i];
    json += "{\"t\":";
    json += ev.timestamp;
    json += ",\"c\":";
    json += ev.channel;
    json += ",\"s\":";
    json += ev.status;
    json += ",\"v\":";
    json += String(ev.value, 1);
    json += ",\"m\":\"";
    json += ev.message;
    json += "\"}";
    if (i < data.eventCount - 1) json += ",";
  }

  json += "]}";
}

FirebaseBridge::FirebaseBridge()
  : nvs(nullptr), sysData(nullptr), dataMutex(nullptr),
    powerCalc(nullptr), limitMgr(nullptr),
    started(false), lastPush(0), lastCommandPoll(0) {}

void FirebaseBridge::begin(NVSManager &nvsRef,
                           SystemData *sysDataRef, SemaphoreHandle_t *mutexRef,
                           PowerCalculator *powerCalcRef, LimitManager *limitMgrRef) {
  nvs = &nvsRef;
  sysData = sysDataRef;
  dataMutex = mutexRef;
  powerCalc = powerCalcRef;
  limitMgr = limitMgrRef;
}

bool FirebaseBridge::configured() const {
  return strlen(FIREBASE_DB_URL) > 0 &&
         strlen(FIREBASE_CLIENT_EMAIL) > 0 &&
         strlen(FIREBASE_PRIVATE_KEY) > 0;
}

void FirebaseBridge::start() {
  if (started || !configured()) return;

  Serial.printf("  %-19s%s\n", "Firebase", "CONFIGURED");

  config.api_key = FIREBASE_API_KEY;
  config.service_account.data.client_email = FIREBASE_CLIENT_EMAIL;
  config.service_account.data.project_id = FIREBASE_PROJECT_ID;
  config.service_account.data.private_key = FIREBASE_PRIVATE_KEY;
  config.database_url = FIREBASE_DB_URL;

  fbdo.setBSSLBufferSize(4096, 4096);
  fbdo.setResponseSize(4096);
  fbCmd.setBSSLBufferSize(4096, 4096);
  fbCmd.setResponseSize(4096);

  config.token_status_callback = tokenStatusCallback;
  Firebase.reconnectNetwork(false);
  Firebase.begin(&config, &auth);

  started = true;
  lastPush = 0;
  Serial.printf("  Firebase bridge started (pushing %s every %u ms, "
                "polling %s every %u ms)\n",
                rtdbPath("latest").c_str(), FIREBASE_PUSH_INTERVAL_MS,
                rtdbPath("commands").c_str(), FIREBASE_COMMAND_POLL_MS);
}

bool FirebaseBridge::ready() const {
  return started && Firebase.ready();
}

void FirebaseBridge::loop() {
  if (!started || !Firebase.ready()) return;

  if (millis() - lastCommandPoll >= FIREBASE_COMMAND_POLL_MS) {
    lastCommandPoll = millis();
    pollCommands();
  }
}

void FirebaseBridge::pushLatest() {
  if (!started || !ready()) return;
  if (millis() - lastPush < FIREBASE_PUSH_INTERVAL_MS) return;
  lastPush = millis();

  String json;
  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  buildSystemJson(*sysData, powerCalc, nvs, json);
  xSemaphoreGive(*dataMutex);

  FirebaseJson payload;
  payload.setJsonData(json.c_str());

  if (Firebase.RTDB.setJSON(&fbdo, rtdbPath("latest"), &payload)) {
    // ok
  } else {
    static uint32_t lastErrLog = 0;
    if (millis() - lastErrLog > 5000) {
      lastErrLog = millis();
      Serial.printf("  [FB] latest push failed: %s\n", fbdo.errorReason().c_str());
    }
  }
}

void FirebaseBridge::pollCommands() {
  if (!ready()) return;

  FirebaseJson cmds;
  if (!Firebase.RTDB.getJSON(&fbCmd, rtdbPath("commands"), &cmds)) {
    String dt = fbCmd.dataType();
    if (dt == "null" || dt.length() == 0) return;
    static uint32_t lastErr = 0;
    if (millis() - lastErr > 10000) {
      lastErr = millis();
      Serial.printf("  [FB] commands poll error: %s\n", fbCmd.errorReason().c_str());
    }
    return;
  }

  size_t n = cmds.iteratorBegin();
  for (size_t i = 0; i < n; i++) {
    int type = 0;
    String key, value;
    cmds.iteratorGet(i, type, key, value);
    if (type == FirebaseJson::JSON_OBJECT || type == FirebaseJson::JSON_STRING) {
      String cmd = value;
      if (cmd.length() > 0 && cmd[0] != '{') {
        cmd = "{\"" + key + "\":" + value + "}";
      }
      String response;
      bool handled = processCommand(nvs, sysData, dataMutex, powerCalc, limitMgr,
                                   cmd.c_str(), &response);
      if (handled) {
        if (response.length() > 0) {
          String outPath = "/devices/";
          outPath += deviceId();
          outPath += "/console/";
          outPath += key;
          if (!Firebase.RTDB.setString(&fbdo, outPath, response)) {
            Serial.printf("  [FB] console response write failed: %s\n",
                          fbdo.errorReason().c_str());
          }
        }
        String path = "/devices/";
        path += deviceId();
        path += "/commands/";
        path += key;
        Firebase.RTDB.deleteNode(&fbdo, path);
      }
    }
  }
  cmds.iteratorEnd();
}

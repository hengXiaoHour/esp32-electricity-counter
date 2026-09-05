#include "firebase_bridge.h"
#include "firebase_config.h"
#include "command_processor.h"
#include "console_handler.h"
#include "../core/limit_manager.h"
#include "../utils/nvs_manager.h"
#include "../utils/device_id.h"
#include "../utils/log_gate.h"

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
  json += ",\"azActive\":";
  json += powerCalc->isAutoZeroActive() ? "true" : "false";
  json += ",\"azChannel\":";
  json += powerCalc->getAutoZeroChannel();
  json += ",\"azProgress\":";
  json += powerCalc->getAutoZeroProgress();
  json += ",\"azQueue\":[";
  {
    int q[NUM_CHANNELS];
    int qLen = powerCalc->getAutoZeroQueue(q, NUM_CHANNELS);
    for (int i = 0; i < qLen; i++) {
      json += q[i];
      if (i < qLen - 1) json += ",";
    }
  }
  json += "]";
  json += ",\"lpfAlpha\":[";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    json += String(powerCalc->lpfAlpha[i], 2);
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
    started(false), lastPush(0), lastCommandPoll(0),
    lastViewerPoll(0), viewerPresent(false) {}

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
  bool apiKeyOk = strlen(FIREBASE_API_KEY) > 10 &&   // AIza<35+ hex>
                  strncmp(FIREBASE_API_KEY, "AIza", 4) == 0;
  bool emailOk = strlen(FIREBASE_AUTH_EMAIL) > 5 &&
                 strstr(FIREBASE_AUTH_EMAIL, "@") != nullptr &&
                 strstr(FIREBASE_AUTH_EMAIL, "PASTE_") == nullptr;
  bool passOk = strlen(FIREBASE_AUTH_PASSWORD) >= 6 &&
                strstr(FIREBASE_AUTH_PASSWORD, "PASTE_") == nullptr;
  bool ok = strlen(FIREBASE_DB_URL) > 0 && apiKeyOk && emailOk && passOk;
  if (!ok)
    Serial.printf("  %-19sWARN: invalid/incomplete API-key or auth credentials — "
                  "Cloud mode disabled (apiKey=%d email=%d pass=%d, db=%d)\n",
                  "Firebase", apiKeyOk ? 1 : 0, emailOk ? 1 : 0,
                  passOk ? 1 : 0, strlen(FIREBASE_DB_URL) > 0 ? 1 : 0);
  return ok;
}

void FirebaseBridge::start() {
  if (started || !configured()) return;

  STATUS_LOG("  %-19s%s\n", "Firebase", "CONFIGURED");

  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_DB_URL;

  auth.user.email = FIREBASE_AUTH_EMAIL;
  auth.user.password = FIREBASE_AUTH_PASSWORD;

  fbdo.setBSSLBufferSize(4096, 4096);
  fbdo.setResponseSize(4096);
  fbCmd.setBSSLBufferSize(4096, 4096);
  fbCmd.setResponseSize(4096);

  config.token_status_callback = tokenStatusCallback;
  Firebase.reconnectNetwork(false);
  Firebase.begin(&config, &auth);

  started = true;
  lastPush = 0;
  consoleErr = "";
  STATUS_LOG("  Firebase bridge started (pushing %s every %u ms, polling %s every %u ms)\n",
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

  if (millis() - lastViewerPoll >= FIREBASE_VIEWER_POLL_MS) {
    lastViewerPoll = millis();
    pollViewers();
  }
}

void FirebaseBridge::pushLatest() {
  if (!started || !ready()) return;
  uint32_t interval = viewerPresent ? FIREBASE_PUSH_INTERVAL_MS
                                    : FIREBASE_ECO_PUSH_INTERVAL_MS;
  if (millis() - lastPush < interval) return;
  lastPush = millis();

  String json;
  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  buildSystemJson(*sysData, powerCalc, nvs, json);
  if (consoleErr.length() > 0) {
    String e;
    e.reserve(consoleErr.length());
    for (size_t i = 0; i < consoleErr.length(); i++) {
      char c = consoleErr[i];
      if (c == '"') e += "\\\"";
      else if (c == '\\') e += "\\\\";
      else if ((unsigned char)c >= 0x20) e += c;
    }
    json = json.substring(0, json.length() - 1) + ",\"fbErr\":\"" + e + "\"}";
  }
  xSemaphoreGive(*dataMutex);

  FirebaseJson payload;
  payload.setJsonData(json.c_str());

  if (Firebase.RTDB.setJSON(&fbdo, rtdbPath("latest"), &payload)) {
    // ok
  } else {
    static uint32_t lastErrLog = 0;
    if (millis() - lastErrLog > 5000) {
      lastErrLog = millis();
      DEBUG_LOG("  [FB] latest push failed: %s\n", fbdo.errorReason().c_str());
    }
  }
}

// Presence check for eco mode: any dashboard holding this board open keeps
// a heartbeat child under /viewers/<clientId> (refreshed every 15 s,
// onDisconnect-removed). Non-empty node = watched. Transient read errors
// keep the last known state so eco doesn't flap on a bad poll.
void FirebaseBridge::pollViewers() {
  if (!ready()) return;

  FirebaseJson v;
  if (!Firebase.RTDB.getJSON(&fbdo, rtdbPath("viewers"), &v)) {
    // Missing node reads back as null (same as pollCommands) — that means
    // the last viewer left, not a transient error.
    String dt = fbdo.dataType();
    if (dt == "null" || dt.length() == 0) setViewerPresent(false);
    return;
  }
  size_t n = v.iteratorBegin();
  v.iteratorEnd();
  setViewerPresent(n > 0);
}

void FirebaseBridge::setViewerPresent(bool present) {
  if (present == viewerPresent) return;
  viewerPresent = present;
  STATUS_LOG("  [FB] viewers %s → Cloud push %s\n",
                present ? "present" : "empty",
                present ? "live (1s)" : "eco (10s)");
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
      DEBUG_LOG("  [FB] commands poll error: %s\n", fbCmd.errorReason().c_str());
    }
    return;
  }

  size_t n = cmds.iteratorBegin();
  for (size_t i = 0; i < n; i++) {
    int type = 0;
    String key, value;
    // FirebaseJson::iteratorGet RECURSIVELY yields top-level objects AND
    // every nested member (child depth > 0). Only the depth-0 value is the
    // actual command object pushed by the dashboard; the flattened children
    // must not be treated as standalone commands (e.g. a pushed object's
    // bare "cmd" field would otherwise be reconstructed as
    // {"cmd":"..."} with no "ch", fooling per-channel commands into acting
    // on every channel).
    int depth = cmds.iteratorGet(i, type, key, value);
    if (depth != 0) continue;
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
          bool ok = false;
          if (Firebase.RTDB.setString(&fbdo, outPath, response)) {
            ok = true;
          } else {
            // setString send failed (heap/SSL path). Fall back to setJSON,
            // which is the primitive proven to work for latest/delete.
            FirebaseJson out;
            out.set("out", response.c_str());
            ok = Firebase.RTDB.setJSON(&fbdo, outPath, &out);
          }
          if (!ok) {
            consoleErr = fbdo.errorReason().c_str();
            DEBUG_LOG("  [FB] console response write failed: %s\n",
                          consoleErr.c_str());
          } else {
            consoleErr = "";
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

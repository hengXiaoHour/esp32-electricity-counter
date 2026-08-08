#include "firebase_bridge.h"
#include "firebase_config.h"
#include "cloud_ota.h"
#include "command_processor.h"
#include "../core/limit_manager.h"
#include "../utils/nvs_manager.h"

#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <time.h>

// ---------------------------------------------------------------
// Shared JSON builder (also used by WebSocketServer)
// ---------------------------------------------------------------
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
  json += ",\"ota\":";
  json += data.otaInProgress ? "true" : "false";
  json += ",\"otaProgress\":";
  json += data.otaProgress;
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

// ---------------------------------------------------------------
// FirebaseBridge
// ---------------------------------------------------------------
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
  // Service-account auth needs db url, client email and private key.
  // The web API key is only used for email/password sign-in — not required.
  return strlen(FIREBASE_DB_URL) > 0 &&
         strlen(FIREBASE_CLIENT_EMAIL) > 0 &&
         strlen(FIREBASE_PRIVATE_KEY) > 0;
}

bool FirebaseBridge::checkOtaTrigger() {
  if (!started || !Firebase.ready()) return false;

  FirebaseJson data;
  if (!Firebase.RTDB.getJSON(&fbdo, "/ota", &data)) return false;

  FirebaseJsonData jd;
  String version, url, md5;
  if (data.get(jd, "version")) version = jd.to<String>();
  if (data.get(jd, "url")) url = jd.to<String>();
  if (data.get(jd, "md5")) md5 = jd.to<String>();

  if (version.isEmpty() || url.isEmpty() || md5.isEmpty()) return false;
  if (md5 == cloudOta.getAppliedMd5()) return false;

  Serial.printf("  [FB] OTA trigger: %s -> %s\n", FIRMWARE_VERSION, version.c_str());
  cloudOta.trigger(version, url, md5);
  return true;
}

void FirebaseBridge::start() {
  if (started || !configured()) return;

  Serial.printf("  %-19s%s\n", "Firebase", "CONFIGURED");

  config.api_key = FIREBASE_API_KEY;
  config.service_account.data.client_email = FIREBASE_CLIENT_EMAIL;
  config.service_account.data.project_id = FIREBASE_PROJECT_ID;
  config.service_account.data.private_key = FIREBASE_PRIVATE_KEY;
  config.database_url = FIREBASE_DB_URL;

  // NOTE: do NOT set auth.token.uid here. Setting it switches the library to
  // the custom-token (signInWithCustomToken) flow, which requires the web API
  // key. Leaving it empty uses the pure OAuth2 service-account flow (JWT ->
  // oauth2.googleapis.com/token), which needs no API key and still yields a
  // valid RTDB auth identity (satisfies rules "auth != null").

  fbdo.setBSSLBufferSize(4096, 4096);
  fbdo.setResponseSize(4096);
  fbCmd.setBSSLBufferSize(4096, 4096);
  fbCmd.setResponseSize(4096);

  config.token_status_callback = tokenStatusCallback;
  Firebase.reconnectNetwork(false);  // WiFi managed by WiFiManager
  Firebase.begin(&config, &auth);

  started = true;
  lastPush = 0;
  Serial.printf("  Firebase bridge started (pushing /latest every %u ms, "
                "polling /commands every %u ms)\n",
                FIREBASE_PUSH_INTERVAL_MS, FIREBASE_COMMAND_POLL_MS);
}

bool FirebaseBridge::ready() const {
  return started && Firebase.ready();
}

void FirebaseBridge::loop() {
  if (!started || !Firebase.ready()) return;

  // Poll the command queue (coarse cadence, cheap).
  if (millis() - lastCommandPoll >= FIREBASE_COMMAND_POLL_MS) {
    lastCommandPoll = millis();
    pollCommands();
  }
}

void FirebaseBridge::pushLatest() {
  if (!started || !ready()) return;
  if (millis() - lastPush < FIREBASE_PUSH_INTERVAL_MS) return;
  lastPush = millis();

  // Snapshot the state under the lock (fast), then RELEASE the mutex before
  // the blocking HTTPS call. Holding the mutex across setJSON would block
  // WebSocket broadcasts (same mutex) for the whole TLS round-trip.
  String json;
  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  buildSystemJson(*sysData, powerCalc, nvs, json);
  xSemaphoreGive(*dataMutex);

  FirebaseJson payload;
  payload.setJsonData(json.c_str());

  if (Firebase.RTDB.setJSON(&fbdo, "/latest", &payload)) {
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
  if (!Firebase.RTDB.getJSON(&fbCmd, "/commands", &cmds)) {
    // Empty (null) node is normal, not an error — and must not tear down the
    // connection (that would force a fresh TLS handshake on the next push).
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
      // Rebuild a full command object: {"cmd":...,"ch":...,"val":...,"name":...}
      // value holds the raw child JSON.
      String cmd = value;
      if (cmd.length() > 0 && cmd[0] != '{') {
        cmd = "{\"" + key + "\":" + value + "}";
      }
      bool handled = processCommand(nvs, sysData, dataMutex, powerCalc, limitMgr,
                                   cmd.c_str());
      if (handled) {
        // Delete the processed command node.
        String path = "/commands/";
        path += key;
        Firebase.RTDB.deleteNode(&fbdo, path);
      }
    }
  }
  cmds.iteratorEnd();
}
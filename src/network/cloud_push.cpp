#include "cloud_push.h"

#include <WiFiClientSecure.h>

#include "../utils/nvs_manager.h"
#include "../utils/log_gate.h"

void CloudPush::begin(NVSManager *nvsRef) {
  nvs = nvsRef;
  String host, auth;
  nvs->loadFb(host, auth);
  // Belt and suspenders: the input verbs validate, but NVS outlives any
  // firmware version, so re-validate what was read before trusting it.
  if (nvs->fbEnabled() && cloud_validateHost(host.c_str()) &&
      cloud_validateAuth(auth.c_str())) {
    host.toCharArray(host_, sizeof(host_));
    auth.toCharArray(auth_, sizeof(auth_));
    enabled_ = true;
  } else {
    enabled_ = false;
  }
  // lastAttemptMs_ starts at "long ago" so the first push goes out ~at once
  // once STA is up, instead of waiting out a full interval after boot.
  lastAttemptMs_ = (uint32_t)(millis() - PUSH_INTERVAL_MS);
  DEBUG_LOG("  [CLOUD] %s\n", enabled_ ? "enabled" : "off (no valid settings)");
}

bool CloudPush::wantsRadio() const {
  return enabled_ && WiFi.status() == WL_CONNECTED;
}

uint32_t CloudPush::secondsSincePush() const {
  if (lastOkMs_ == 0) return UINT32_MAX;
  return (millis() - lastOkMs_) / 1000;
}

// Copies the live readings into a small cloud payload. Runs UNDER dataMutex
// (taken here, 50 ms max) so the numbers are mutually consistent; the TLS
// that follows runs WITHOUT it.
bool CloudPush::snapshot(SystemData *sysData, SemaphoreHandle_t *mutex, String &body) {
  if (!sysData || !mutex) return false;
  bool locked = (xSemaphoreTake(*mutex, pdMS_TO_TICKS(50)) == pdTRUE);

  float v = sysData->voltageRMS;
  uint32_t up = sysData->uptime;
  int8_t rssi = sysData->wifiRSSI;
  float mcu = sysData->mcuTempC;
  bool eco = sysData->ecoMode;
  float w[NUM_CHANNELS], kwh[NUM_CHANNELS];
  uint8_t st[NUM_CHANNELS];
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    w[ch] = sysData->channels[ch].activePower;
    kwh[ch] = sysData->channels[ch].energyKWh;
    st[ch] = (uint8_t)sysData->channels[ch].status;
  }

  if (locked) xSemaphoreGive(*mutex);

  // MAC-derived id, recomputed per push: cheap, and always the radio's truth
  // rather than a cached copy from boot.
  deviceId_[0] = '\0';
  String mac = WiFi.macAddress();
  if (!cloud_formatDeviceId(mac.c_str(), deviceId_)) {
    deviceId_[0] = '\0';
    return false;
  }

  body = "{\"dev\":\"";
  body += deviceId_;
  body += "\",\"epoch\":";
  body += (long)time(nullptr);
  body += ",\"uptime\":";
  body += up;
  body += ",\"rssi\":";
  body += rssi;
  body += ",\"v\":";
  body += String(v, 1);
  // Same null-not-nan rule as the dashboard snapshot: a bare nan is invalid
  // JSON and would make the database store a broken row.
  body += ",\"mcu\":";
  if (isnan(mcu)) body += "null";
  else body += String(mcu, 1);
  body += ",\"eco\":";
  body += eco ? "true" : "false";
  body += ",\"ch\":[";
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    body += "{\"w\":";
    body += String(w[ch], 1);
    body += ",\"kwh\":";
    body += String(kwh[ch], 3);
    body += ",\"s\":";
    body += st[ch];
    body += "}";
    if (ch < NUM_CHANNELS - 1) body += ",";
  }
  body += "]}";
  return true;
}

// One PATCH, one fresh connection, no reads beyond the status line. Returns
// true only on " 200 ". The token travels inside the TLS tunnel; it is never
// logged, never stored anywhere but NVS, and never rendered.
bool CloudPush::post(const String &body) {
  WiFiClientSecure client;
  client.setInsecure();  // a power meter pushing its own readings with a token;
                         // cert validation would add a clock dependency for no
                         // meaningful gain (same threat model as the plaintext
                         // admin PIN: physical access already owns the board).
  client.setTimeout(4000);

  if (!client.connect(host_, 443)) {
    DEBUG_LOG("  [CLOUD] connect failed (%s)\n", host_);
    return false;
  }

  String req = "PATCH /devices/";
  req += deviceId_;
  req += "/latest.json?auth=";
  req += auth_;
  req += " HTTP/1.1\r\nHost: ";
  req += host_;
  req += "\r\nContent-Type: application/json\r\nContent-Length: ";
  req += body.length();
  req += "\r\nConnection: close\r\n\r\n";
  client.print(req);
  client.print(body);

  String status = client.readStringUntil('\n');
  client.stop();
  bool ok = status.indexOf(" 200 ") >= 0;
  if (!ok) {
    // Status line only, truncated: enough to distinguish 401 (bad token)
    // from 404/400 (bad path), never the body, never the token.
    status.trim();
    if (status.length() > 48) status = status.substring(0, 48);
    DEBUG_LOG("  [CLOUD] push refused: %s\n", status.c_str());
  }
  return ok;
}

void CloudPush::loop(SystemData *sysData, SemaphoreHandle_t *mutex) {
  if (!enabled_) return;
  if (millis() - lastAttemptMs_ < PUSH_INTERVAL_MS) return;
  lastAttemptMs_ = millis();  // set BEFORE the attempt: a hanging TLS must not
                              // rapid-retry, and a down link must not spam.
  if (WiFi.status() != WL_CONNECTED) return;  // STA-only: the fallback AP has
                                              // no internet by definition.
  String body;
  if (!snapshot(sysData, mutex, body)) return;
  if (post(body)) {
    lastOk_ = true;
    lastOkMs_ = millis();
  } else {
    lastOk_ = false;
  }
}

#include "cloud_push.h"

#include <WiFiClientSecure.h>

#include "../utils/nvs_manager.h"
#include "../utils/log_gate.h"
#include "console_handler.h"  // consoleAppendf: diag() reports through it

// Extracts "key": "value" (or "key":123 for numbers when wantStr is false)
// from a flat JSON object. No JSON library on purpose: the auth responses are
// ~1-2 KB flat objects and the classic ESP32 is at 69% flash without one.
static bool extractField(const String &body, const char *key, char *out, size_t outLen) {
  if (!out || outLen == 0) return false;
  String needle = "\"";
  needle += key;
  needle += "\":";
  int i = body.indexOf(needle);
  if (i < 0) return false;
  i += needle.length();
  while (i < (int)body.length() && (body[i] == ' ' || body[i] == '"')) {
    if (body[i] == '"') { i++; break; }
    i++;
  }
  int j = i;
  while (j < (int)body.length() && body[j] != '"' && body[j] != ',' && body[j] != '}') j++;
  // Trailing quote consumed: value ran to a closing quote.
  int len = j - i;
  if (len <= 0 || (size_t)len >= outLen) return false;
  memcpy(out, body.c_str() + i, len);
  out[len] = '\0';
  return true;
}

// POSTs `path`+body to `host:443` and returns the HTTP status code,
// 0 on transport failure. Reads the status line plus a bounded body for
// field extraction; nothing else is parsed, nothing is polled.
static int httpsPost(const char *host, const String &path, const String &body,
                     const char *contentType, String &respBody) {
  WiFiClientSecure client;
  client.setInsecure();  // same threat model as before: physical access owns
                         // the board; cert validation buys nothing here.
  client.setTimeout(4000);
  if (!client.connect(host, 443)) return 0;

  client.print(String("POST ") + path + " HTTP/1.1\r\nHost: " + host +
               "\r\nContent-Type: " + contentType + "\r\nContent-Length: " +
               body.length() + "\r\nConnection: close\r\n\r\n");
  client.print(body);

  String status = client.readStringUntil('\n');
  int code = 0;
  int sp = status.indexOf(' ');
  if (sp >= 0) code = status.substring(sp + 1).toInt();

  // Skip headers, then read a bounded body (auth payloads are ~1-2 KB).
  respBody = "";
  unsigned long deadline = millis() + 4000;
  bool inBody = false;
  while (client.connected() && millis() < deadline && respBody.length() < 2048) {
    if (!inBody) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) inBody = true;
      continue;
    }
    while (client.available() && respBody.length() < 2048) {
      respBody += (char)client.read();
    }
    if (!client.available()) break;
  }
  client.stop();
  return code;
}

void CloudPush::begin(NVSManager *nvsRef) {
  nvs = nvsRef;
  String host, email, pass;
  nvs->loadFb(host, email, pass);
  // Belt and suspenders: the input verbs validate, but NVS outlives any
  // firmware version, so re-validate what was read before trusting it.
  if (nvs->fbEnabled() && cloud_validateHost(host.c_str()) &&
      cloud_validateEmail(email.c_str()) && cloud_validatePass(pass.c_str())) {
    host.toCharArray(host_, sizeof(host_));
    email.toCharArray(email_, sizeof(email_));
    pass.toCharArray(pass_, sizeof(pass_));
    enabled_ = true;
  } else {
    enabled_ = false;
  }
  // lastAttemptMs_ starts at "long ago" so the first push goes out ~at once
  // once STA is up, instead of waiting out a full interval after boot.
  lastAttemptMs_ = (uint32_t)(millis() - PUSH_INTERVAL_MS);
  DEBUG_LOG("  [CLOUD] %s\n", enabled_ ? "enabled" : "off (no valid account)");
}

bool CloudPush::wantsRadio() const {
  return enabled_ && WiFi.status() == WL_CONNECTED;
}

uint32_t CloudPush::secondsSincePush() const {
  if (lastOkMs_ == 0) return UINT32_MAX;
  return (millis() - lastOkMs_) / 1000;
}

bool CloudPush::signIn() {
  String path = "/v1/accounts:signInWithPassword?key=";
  path += CLOUD_API_KEY_DEFAULT;
  String req = "{\"email\":\"";
  req += email_;
  req += "\",\"password\":\"";
  req += pass_;
  req += "\",\"returnSecureToken\":true}";
  String resp;
  // Host is fixed DNS (googleapis.com), not a setting - no validation needed.
  int code = httpsPost("identitytoolkit.googleapis.com", path, req,
                       "application/json", resp);
  if (code != 200) {
    // INVALID_LOGIN_CREDENTIALS vs too many attempts vs disabled account are
    // all actionable WITHOUT the password, which is never printed.
    DEBUG_LOG("  [CLOUD] sign-in refused (HTTP %d) for \"%s\"\n", code, email_);
    idToken_[0] = '\0';
    refreshToken_[0] = '\0';
    tokenExpiryMs_ = 0;
    return false;
  }
  char idt[sizeof(idToken_)], rft[sizeof(refreshToken_)], exp[16];
  if (!extractField(resp, "idToken", idt, sizeof(idt)) ||
      !extractField(resp, "refreshToken", rft, sizeof(rft)) ||
      !extractField(resp, "expiresIn", exp, sizeof(exp))) {
    DEBUG_LOG("  [CLOUD] sign-in reply unparseable\n");
    return false;
  }
  memcpy(idToken_, idt, sizeof(idToken_));
  memcpy(refreshToken_, rft, sizeof(refreshToken_));
  // Refresh 5 min early: a push must never race expiry mid-PATCH.
  long ttl = atol(exp);
  if (ttl < 600) ttl = 600;
  tokenExpiryMs_ = millis() + (uint32_t)(ttl - 300) * 1000UL;
  DEBUG_LOG("  [CLOUD] signed in as \"%s\" (token %.0fs)\n", email_, (double)ttl);
  return true;
}

bool CloudPush::refresh() {
  if (refreshToken_[0] == '\0') return false;
  String path = "/v1/token?key=";
  path += CLOUD_API_KEY_DEFAULT;
  String req = "grant_type=refresh_token&refresh_token=";
  req += refreshToken_;
  String resp;
  int code = httpsPost("securetoken.googleapis.com", path, req,
                       "application/x-www-form-urlencoded", resp);
  if (code != 200) {
    DEBUG_LOG("  [CLOUD] token refresh refused (HTTP %d)\n", code);
    idToken_[0] = '\0';
    refreshToken_[0] = '\0';
    tokenExpiryMs_ = 0;
    return false;
  }
  char idt[sizeof(idToken_)], rft[sizeof(refreshToken_)], exp[16];
  if (!extractField(resp, "id_token", idt, sizeof(idt)) ||
      !extractField(resp, "refresh_token", rft, sizeof(rft)) ||
      !extractField(resp, "expires_in", exp, sizeof(exp))) {
    DEBUG_LOG("  [CLOUD] refresh reply unparseable\n");
    return false;
  }
  memcpy(idToken_, idt, sizeof(idToken_));
  memcpy(refreshToken_, rft, sizeof(rft));
  long ttl = atol(exp);
  if (ttl < 600) ttl = 600;
  tokenExpiryMs_ = millis() + (uint32_t)(ttl - 300) * 1000UL;
  return true;
}

bool CloudPush::ensureLogin() {
  // Token still fresh: nothing to do (the common path - one comparison).
  if (idToken_[0] != '\0' && (int32_t)(millis() - tokenExpiryMs_) < 0) return true;
  // Stale but refreshable: one cheap call, no password involved.
  if (refreshToken_[0] != '\0' && refresh()) return true;
  // Otherwise (first boot, expiry lapsed, refresh rejected): full sign-in.
  return signIn();
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
  // JSON, and now the database .validate would reject the row too.
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

// One PATCH, one fresh connection, no reads beyond the status line. The ID
// token travels inside the TLS tunnel; it is never logged, never stored
// anywhere but RAM, and never rendered. Returns the HTTP status so the caller
// can tell 401 (re-login) from failure (log and forget).
int CloudPush::postStatus(const String &body) {
  WiFiClientSecure client;
  client.setInsecure();  // same threat model as the auth calls above.
  client.setTimeout(4000);

  if (!client.connect(host_, 443)) {
    DEBUG_LOG("  [CLOUD] connect failed (%s)\n", host_);
    return 0;
  }

  String req = "PATCH /devices/";
  req += deviceId_;
  req += "/latest.json?auth=";
  req += idToken_;
  req += " HTTP/1.1\r\nHost: ";
  req += host_;
  req += "\r\nContent-Type: application/json\r\nContent-Length: ";
  req += body.length();
  req += "\r\nConnection: close\r\n\r\n";
  client.print(req);
  client.print(body);

  String status = client.readStringUntil('\n');
  client.stop();
  int sp = status.indexOf(' ');
  return (sp >= 0) ? status.substring(sp + 1).toInt() : 0;
}

bool CloudPush::post(const String &body) {
  int code = postStatus(body);
  if (code == 401) {
    // Token died mid-interval (revoked, or the hour lapsed early). One
    // fresh sign-in and ONE retry - never a loop: a second 401 means the
    // account itself is wrong, and retrying is spam.
    DEBUG_LOG("  [CLOUD] push got 401 - re-login once\n");
    idToken_[0] = '\0';
    if (!signIn()) return false;
    code = postStatus(body);
  }
  if (code != 200) {
    DEBUG_LOG("  [CLOUD] push refused (HTTP %d)\n", code);
    return false;
  }
  return true;
}

void CloudPush::loop(SystemData *sysData, SemaphoreHandle_t *mutex) {
  if (!enabled_) return;
  if (millis() - lastAttemptMs_ < PUSH_INTERVAL_MS) return;
  lastAttemptMs_ = millis();  // set BEFORE the attempt: hanging TLS must not
                              // rapid-retry, and a down link must not spam.
  if (WiFi.status() != WL_CONNECTED) return;  // STA-only: the fallback AP has
                                              // no internet by definition.
  if (!ensureLogin()) { lastOk_ = false; return; }
  String body;
  if (!snapshot(sysData, mutex, body)) return;
  if (post(body)) {
    lastOk_ = true;
    lastOkMs_ = millis();
  } else {
    lastOk_ = false;
  }
}

void CloudPush::diag(String &out, SemaphoreHandle_t *mutex) {
  // Snapshot the session under the mutex: the WS console runs on a different
  // task than loop(), and a torn read here would report a "mangled" token
  // that is really just bytes mid-write.
  char tok[sizeof(idToken_)];
  tok[0] = '\0';
  bool locked = (mutex && xSemaphoreTake(*mutex, pdMS_TO_TICKS(50)) == pdTRUE);
  if (idToken_[0] != '\0') memcpy(tok, idToken_, sizeof(tok));
  uint32_t expMs = tokenExpiryMs_;
  if (locked) xSemaphoreGive(*mutex);

  consoleAppendf(out, "  %-10s%s", "Enabled:", enabled_ ? "yes" : "no");
  consoleAppendf(out, "  %-10s\"%s\"", "Host:", host_);
  consoleAppendf(out, "  %-10s\"%s\"", "Account:", email_);
  consoleAppendf(out, "  %-10s\"%s\"", "Device:", deviceId_);
  if (tok[0] == '\0') {
    consoleAppendf(out, "%s", "  Session: none (no sign-in yet, or refresh lapsed)");
    return;
  }
  size_t tlen = strlen(tok);
  char aud[96];
  aud[0] = '\0';
  bool audOk = cloud_jwtAud(tok, aud, sizeof(aud));
  // Length + audience ONLY. A prefix/suffix sample would still leak enough
  // for correlation, and buys nothing over the length for a mangling check:
  // a truncated or corrupted token fails the aud parse or shows a wrong/short
  // length, which is exactly the verdict this command exists to give.
  consoleAppendf(out, "  %-10s%u chars (%s)", "Token:",
                 (unsigned)tlen, audOk ? "aud ok" : "aud UNPARSEABLE");
  if (audOk) consoleAppendf(out, "  %-10s\"%s\"", "Aud:", aud);
  if (expMs != 0) {
    int32_t left = (int32_t)(expMs - millis());
    consoleAppendf(out, "  %-10s%s (%ld s left)", "Expiry:",
                   left > 0 ? "valid" : "STALE", (long)(left / 1000));
  }
  // The token and password are deliberately unprintable here: there is no
  // code path in this function that formats either. A check_docs rule
  // asserts that property against the source, not just this comment.
}

#include "cloud_push.h"

#include <WiFiClientSecure.h>
#include <time.h>

#include "../utils/nvs_manager.h"
#include "../utils/log_gate.h"
#include "command_processor.h"  // downlink executes cloud frames as commands
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

// GETs `path` from `host:443` and returns the HTTP status code,
// 0 on transport failure. Reads the status line plus a bounded body (the cmd
// node is small: {id, frame, ts} where frame is one dashboard command).
// Downlink-only: the push path above never reads.
static int httpsGet(const char *host, const String &path, String &respBody,
                    size_t maxLen) {
  WiFiClientSecure client;
  client.setInsecure();  // same threat model as the auth calls above.
  client.setTimeout(4000);
  if (!client.connect(host, 443)) return 0;

  client.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
               "\r\nConnection: close\r\n\r\n");

  String status = client.readStringUntil('\n');
  int code = 0;
  int sp = status.indexOf(' ');
  if (sp >= 0) code = status.substring(sp + 1).toInt();

  // Skip headers, then read a bounded body (the cmd node is one small
  // object; maxLen caps a runaway while leaving real commands room).
  respBody = "";
  unsigned long deadline = millis() + 4000;
  bool inBody = false;
  while (client.connected() && millis() < deadline && respBody.length() < maxLen) {
    if (!inBody) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) inBody = true;
      continue;
    }
    while (client.available() && respBody.length() < maxLen) {
      respBody += (char)client.read();
    }
    if (!client.available()) break;
  }
  client.stop();
  return code;
}

// PATCHes `path`+body to `host:443` and returns the HTTP status code,
// 0 on transport failure. The ack write (small JSON object) only needs the
// status line back.
static int httpsPatch(const char *host, const String &path, const String &body,
                      String &respBody) {
  WiFiClientSecure client;
  client.setInsecure();  // same threat model as the auth calls above.
  client.setTimeout(4000);
  if (!client.connect(host, 443)) return 0;

  client.print(String("PATCH ") + path + " HTTP/1.1\r\nHost: " + host +
               "\r\nContent-Type: application/json\r\nContent-Length: " +
               body.length() + "\r\nConnection: close\r\n\r\n");
  client.print(body);

  String status = client.readStringUntil('\n');
  int code = 0;
  int sp = status.indexOf(' ');
  if (sp >= 0) code = status.substring(sp + 1).toInt();

  respBody = "";
  unsigned long deadline = millis() + 4000;
  bool inBody = false;
  while (client.connected() && millis() < deadline && respBody.length() < 1024) {
    if (!inBody) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) inBody = true;
      continue;
    }
    while (client.available() && respBody.length() < 1024) {
      respBody += (char)client.read();
    }
    if (!client.available()) break;
  }
  client.stop();
  return code;
}

// Reverses the JSON string escaping on a quoted value (the cmd frame arrives
// as an escaped JSON string inside the cmd node).
static String cloudUnescape(const String &s) {
  String r;
  r.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' && i + 1 < s.length()) {
      char n = s[++i];
      if (n == 'n') r += '\n';
      else if (n == 't') r += '\t';
      else if (n == 'r') r += '\r';
      else r += n;  // covers \" \\ \/
    } else {
      r += c;
    }
  }
  return r;
}

// Extracts "key":"value" honouring backslash escapes (a plain scan stops at
// the first \" inside the frame and returns a truncated command).
static bool cloudExtractStr(const String &s, const char *key, String &outVal) {
  String needle = String("\"") + key + "\":\"";
  int i = s.indexOf(needle);
  if (i < 0) return false;
  i += needle.length();
  int end = i;
  while (end < (int)s.length()) {
    if (s[end] == '\\') { end += 2; continue; }
    if (s[end] == '"') break;
    end++;
  }
  if (end > (int)s.length()) return false;
  outVal = cloudUnescape(s.substring(i, end));
  return true;
}

// Escapes arbitrary output text for embedding in the ack JSON string.
static String cloudEscapeOut(const String &s) {
  String r;
  r.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') r += "\\\"";
    else if (c == '\\') r += "\\\\";
    else if (c == '\n') r += "\\n";
    else if (c == '\r') r += "\\r";
    else if (c == '\t') r += "\\t";
    else if ((uint8_t)c < 0x20) continue;
    else r += c;
  }
  return r;
}

// Reads the "cmd" verb out of an already-unescaped frame for the log line.
// Returns false when the frame has no verb (still executed - the ack says
// ignored - just logged as "?").
static bool cloudExtractVerb(const String &frame, char *verb, size_t verbLen) {
  if (!verb || verbLen == 0) return false;
  const char *needle = "\"cmd\":\"";
  int i = frame.indexOf(needle);
  if (i < 0) return false;
  i += 7;
  int j = frame.indexOf('"', i);
  if (j <= i) return false;
  size_t n = (size_t)(j - i);
  if (n + 1 > verbLen) return false;
  memcpy(verb, frame.c_str() + i, n);
  verb[n] = '\0';
  return true;
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
  // Same for the downlink timer: the first cmd poll goes out on the first
  // loop tick (its own 10 s timer, so push and poll TLS never share timing).
  lastPollMs_ = (uint32_t)(millis() - POLL_INTERVAL_MS);
  // Dedup across reboots: an acked command id stays executed even if the
  // board restarts before the admin clears the cmd node.
  lastCmdId_[0] = '\0';
  String cmdId;
  if (nvs->loadCloudCmdId(cmdId)) {
    strncpy(lastCmdId_, cmdId.c_str(), sizeof(lastCmdId_) - 1);
    lastCmdId_[sizeof(lastCmdId_) - 1] = '\0';
  }
  DEBUG_LOG("  [CLOUD] %s\n", enabled_ ? "enabled" : "off (no valid account)");
}

bool CloudPush::wantsRadio() const {
  return enabled_ && WiFi.status() == WL_CONNECTED;
}

uint32_t CloudPush::secondsSincePush() const {
  if (lastOkMs_ == 0) return UINT32_MAX;
  return (millis() - lastOkMs_) / 1000;
}

uint32_t CloudPush::secondsSincePoll() const {
  if (lastPollMs_ == 0) return UINT32_MAX;
  return (millis() - lastPollMs_) / 1000;
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

// One downlink poll: fetch the admin's cmd node, execute a NEW id once,
// ack it. TLS runs with dataMutex RELEASED throughout: snapshot() and
// processCommand() take it briefly themselves, and only the final id record
// below holds it (50 ms take, same pattern as snapshot).
void CloudPush::pollCmd(SystemData *sysData, SemaphoreHandle_t *mutex,
                        PowerCalculator *powerCalc, LimitManager *limitMgr,
                        NVSManager *nvsRef) {
  if (!enabled_) return;
  if (WiFi.status() != WL_CONNECTED) return;  // STA-only: the fallback AP
                                              // has no internet by definition.
  if (!ensureLogin()) return;  // one sign-in/refresh inside; never loops.

  // Device id: the radio's truth, recomputed when unknown like the push does.
  if (deviceId_[0] == '\0') {
    String mac = WiFi.macAddress();
    if (!cloud_formatDeviceId(mac.c_str(), deviceId_)) {
      deviceId_[0] = '\0';
      return;
    }
  }

  String getPath = "/devices/";
  getPath += deviceId_;
  getPath += "/cmd.json?auth=";
  getPath += idToken_;
  String cmdBody;
  int code = httpsGet(host_, getPath, cmdBody, 4096);
  if (code == 401) {
    // Token died mid-interval. One fresh sign-in and ONE retry - never a
    // loop, same rule as post().
    DEBUG_LOG("  [CLOUD] cmd poll got 401 - re-login once\n");
    idToken_[0] = '\0';
    if (!signIn()) return;
    getPath = "/devices/";
    getPath += deviceId_;
    getPath += "/cmd.json?auth=";
    getPath += idToken_;
    code = httpsGet(host_, getPath, cmdBody, 4096);
  }
  if (code != 200) {
    if (code == 0) DEBUG_LOG("  [CLOUD] cmd poll connect failed\n");
    else DEBUG_LOG("  [CLOUD] cmd poll refused (HTTP %d)\n", code);
    return;
  }
  // "null" (node never written) and {} both mean nothing new: no id parses.
  String cmdId, frame;
  if (!cloudExtractStr(cmdBody, "id", cmdId) || cmdId.length() == 0) return;
  if (!cloudExtractStr(cmdBody, "frame", frame) || frame.length() == 0) return;
  // Defensive cap: lastCmdId_ is 48 bytes, so an over-long admin id can never
  // compare equal to its own truncated record (which would re-run forever).
  if (cmdId.length() >= sizeof(lastCmdId_)) cmdId.remove(sizeof(lastCmdId_) - 1);
  if (cmdId.equals(lastCmdId_)) return;  // already executed

  // Execute WITHOUT the PIN: trust comes from the RTDB rules (only the admin
  // Gmail can write cmd) + TLS + this ID token. The frame may carry
  // set_pin/set_ap/setwifi passwords, so it is NEVER logged - verb + id only.
  char verb[32] = {0};
  cloudExtractVerb(frame, verb, sizeof(verb));
  String out;
  bool ok = processCommand(nvsRef, sysData, mutex, powerCalc, limitMgr,
                           frame.c_str(), &out, nullptr, true);
  if (out.length() > 200) out.remove(200);  // the ack stays a small object
  DEBUG_LOG("  [CLOUD] cmd '%s' id '%.20s' -> %s\n", verb[0] ? verb : "?",
            cmdId.c_str(), ok ? "ok" : "ignored");

  String ackBody = "{\"id\":\"";
  ackBody += cloudEscapeOut(cmdId);
  ackBody += "\",\"ok\":";
  ackBody += ok ? "true" : "false";
  ackBody += ",\"ts\":";
  ackBody += (long)time(nullptr);
  ackBody += ",\"out\":\"";
  ackBody += cloudEscapeOut(out);
  ackBody += "\"}";
  String ackPath = "/devices/";
  ackPath += deviceId_;
  ackPath += "/ack.json?auth=";
  ackPath += idToken_;
  String ackResp;
  int ackCode = httpsPatch(host_, ackPath, ackBody, ackResp);
  if (ackCode == 401) {
    DEBUG_LOG("  [CLOUD] cmd ack got 401 - re-login once\n");
    idToken_[0] = '\0';
    if (!signIn()) return;
    ackPath = "/devices/";
    ackPath += deviceId_;
    ackPath += "/ack.json?auth=";
    ackPath += idToken_;
    ackCode = httpsPatch(host_, ackPath, ackBody, ackResp);
  }
  if (ackCode != 200) {
    DEBUG_LOG("  [CLOUD] cmd ack refused (HTTP %d)\n", ackCode);
    return;  // id NOT recorded: the next poll retries the same command.
  }
  // Record under the mutex: commit() is prefs.end()+prefs.begin(), which is
  // not thread-safe against sensorTask's 5 s energy save on the same handle.
  bool locked = (mutex && xSemaphoreTake(*mutex, pdMS_TO_TICKS(50)) == pdTRUE);
  strncpy(lastCmdId_, cmdId.c_str(), sizeof(lastCmdId_) - 1);
  lastCmdId_[sizeof(lastCmdId_) - 1] = '\0';
  nvsRef->saveCloudCmdId(cmdId);
  nvsRef->commit();
  if (locked) xSemaphoreGive(*mutex);
}

void CloudPush::loop(SystemData *sysData, SemaphoreHandle_t *mutex,
                     PowerCalculator *powerCalc, LimitManager *limitMgr) {
  if (!enabled_) return;
  uint32_t now = millis();
  if (now - lastAttemptMs_ >= PUSH_INTERVAL_MS) {
    lastAttemptMs_ = now;  // set BEFORE the attempt: hanging TLS must not
                           // rapid-retry, and a down link must not spam.
    if (WiFi.status() == WL_CONNECTED) {  // STA-only: the fallback AP has
                                           // no internet by definition.
      if (!ensureLogin()) { lastOk_ = false; }
      else {
        String body;
        if (snapshot(sysData, mutex, body)) {
          if (post(body)) {
            lastOk_ = true;
            lastOkMs_ = now;
          } else {
            lastOk_ = false;
          }
        }
      }
    }
  }
  // Downlink on its own 10 s timer so a slow push TLS never starves a poll
  // (and vice versa). The STA + login gate lives inside pollCmd.
  if (now - lastPollMs_ >= POLL_INTERVAL_MS) {
    lastPollMs_ = now;
    pollCmd(sysData, mutex, powerCalc, limitMgr, nvs);
  }
}

void CloudPush::diag(String &out, SemaphoreHandle_t *mutex) {
  // Snapshot the session under the mutex: the WS console runs on a different
  // task than loop(), and a torn read here would report a "mangled" token
  // that is really just bytes mid-write.
  char tok[sizeof(idToken_)];
  tok[0] = '\0';
  char cmd[sizeof(lastCmdId_)];
  cmd[0] = '\0';
  bool locked = (mutex && xSemaphoreTake(*mutex, pdMS_TO_TICKS(50)) == pdTRUE);
  if (idToken_[0] != '\0') memcpy(tok, idToken_, sizeof(tok));
  if (lastCmdId_[0] != '\0') memcpy(cmd, lastCmdId_, sizeof(cmd));
  uint32_t expMs = tokenExpiryMs_;
  if (locked) xSemaphoreGive(*mutex);
  // Single-cycle aligned read like the push age above it.
  uint32_t pollAgeS = secondsSincePoll();

  consoleAppendf(out, "  %-10s%s", "Enabled:", enabled_ ? "yes" : "no");
  consoleAppendf(out, "  %-10s\"%s\"", "Host:", host_);
  consoleAppendf(out, "  %-10s\"%s\"", "Account:", email_);
  consoleAppendf(out, "  %-10s\"%s\"", "Device:", deviceId_);
  // Last executed downlink id (first 20 chars) + poll age. The id is an
  // opaque admin-chosen tag, not a secret; the frame it carried is never
  // shown here (it may hold passwords).
  consoleAppendf(out, "  %-10s\"%.20s\"%s", "LastCmd:", cmd,
                 cmd[0] ? "" : " (none yet)");
  if (pollAgeS == UINT32_MAX) {
    consoleAppendf(out, "%s", "  Poll: never");
  } else {
    consoleAppendf(out, "  %-10s%lus ago", "Poll:", (unsigned long)pollAgeS);
  }
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

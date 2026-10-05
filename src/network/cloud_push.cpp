#include "cloud_push.h"

#include <WiFiClientSecure.h>
#include <time.h>

#include "../utils/nvs_manager.h"
#include "../utils/log_gate.h"
#include "time_sync.h"
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

// Persistent TLS connection shared by every cloud HTTPS call below.
//
// One CloudPush instance exists on this board, so translation-unit state is
// honest here, not a shortcut: a member would say "per instance" what is
// really "per radio". It exists because a fresh handshake costs 1-2 s on
// this silicon, which made 1 s pushes arithmetically impossible - the
// handshake outlasted the interval and networkTask never came up for air.
// One handshake at boot (plus one more whenever the host switches between
// the auth endpoints and the database), then each push/poll is a ~100 ms
// round trip on the hot connection.
static WiFiClientSecure s_conn;
static char s_connHost[160] = {0};
static bool s_connOpen = false;

static void cloudDropConn() {
  s_conn.stop();
  s_connOpen = false;
  s_connHost[0] = '\0';
}

// True with a live TLS session to `host`. Reconnects when closed or when
// the host switched (auth endpoints vs the database host).
static bool cloudEnsureConn(const char *host) {
  if (s_connOpen && strcmp(s_connHost, host) == 0 && s_conn.connected()) {
    return true;
  }
  cloudDropConn();
  s_conn.setInsecure();  // same threat model as before: physical access owns
                         // the board; cert validation buys nothing here.
  s_conn.setTimeout(4000);
  if (!s_conn.connect(host, 443)) {
    return false;
  }
  strncpy(s_connHost, host, sizeof(s_connHost) - 1);
  s_connHost[sizeof(s_connHost) - 1] = '\0';
  s_connOpen = true;
  return true;
}

// Reads a newline-terminated line before `deadline`, else false.
static bool cloudReadLine(String &line, unsigned long deadline) {
  while ((long)(deadline - millis()) > 0) {
    if (s_conn.available()) {
      line = s_conn.readStringUntil('\n');
      return true;
    }
    delay(1);
  }
  return false;
}

// One request on the persistent connection. Returns the HTTP status code,
// 0 on transport failure (connection dropped, next call reconnects).
// Retried ONCE, and only when the first attempt rode a reused connection:
// a server idle-close lands exactly there, while a fresh connection failing
// means the network itself is down and an instant second handshake is spam.
static int cloudRoundTrip(const char *host, const String &req,
                          String &respBody, size_t maxLen) {
  for (int attempt = 0; attempt < 2; attempt++) {
    bool fresh = !s_connOpen;
    if (!cloudEnsureConn(host)) {
      return 0;
    }
    s_conn.print(req);
    unsigned long deadline = millis() + 4000;
    String status;
    if (!cloudReadLine(status, deadline)) {
      cloudDropConn();
      if (!fresh) continue;
      return 0;
    }
    int code = 0;
    int sp = status.indexOf(' ');
    if (sp >= 0) code = status.substring(sp + 1).toInt();
    if (code <= 0) {
      cloudDropConn();
      if (!fresh) continue;
      return 0;
    }
    // Headers: only Content-Length matters. Anything without one
    // (chunked, close-delimited) cannot be resynced, so the connection is
    // dropped instead of guessed at - these endpoints always send one.
    long want = -1;
    bool hdrFail = false;
    while (true) {
      String h;
      if (!cloudReadLine(h, deadline)) {
        hdrFail = true;
        break;
      }
      h.trim();
      if (h.length() == 0) break;
      if (h.length() > 15 && strncasecmp(h.c_str(), "content-length:", 15) == 0) {
        want = atol(h.c_str() + 15);
      }
    }
    if (hdrFail) {
      cloudDropConn();
      if (!fresh) continue;
      return 0;
    }
    if (want < 0) {
      cloudDropConn();
      return 0;
    }
    // Bounded body, then the stream is exactly back in sync. A body larger
    // than maxLen is truncated AND the connection dropped (the unread tail
    // would otherwise poison the next response).
    respBody = "";
    long n = want;
    bool over = false;
    if (n > (long)maxLen) {
      n = maxLen;
      over = true;
    }
    while (n > 0 && (long)(deadline - millis()) > 0) {
      if (!s_conn.available()) {
        delay(1);
        continue;
      }
      respBody += (char)s_conn.read();
      n--;
    }
    if (n > 0) {
      cloudDropConn();
      if (!fresh) continue;
      return 0;
    }
    if (over) cloudDropConn();
    return code;
  }
  return 0;
}

// POSTs `path`+body to `host:443` and returns the HTTP status code,
// 0 on transport failure. Reads the status line plus a bounded body for
// field extraction; nothing else is parsed, nothing is polled.
static int httpsPost(const char *host, const String &path, const String &body,
                     const char *contentType, String &respBody) {
  String req = String("POST ") + path + " HTTP/1.1\r\nHost: " + host +
               "\r\nContent-Type: " + contentType + "\r\nContent-Length: " +
               body.length() + "\r\nConnection: keep-alive\r\n\r\n";
  req += body;
  // Auth payloads are ~1-2 KB; the bound is what keeps a lying server from
  // eating the heap, not what fits a real reply.
  return cloudRoundTrip(host, req, respBody, 2048);
}

// GETs `path` from `host:443` and returns the HTTP status code,
// 0 on transport failure. Reads the status line plus a bounded body (the cmd
// node is small: {id, frame, ts} where frame is one dashboard command).
// Downlink-only: the push path above never reads.
static int httpsGet(const char *host, const String &path, String &respBody,
                    size_t maxLen) {
  String req = String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
               "\r\nConnection: keep-alive\r\n\r\n";
  return cloudRoundTrip(host, req, respBody, maxLen);
}

// PATCHes `path`+body to `host:443` and returns the HTTP status code,
// 0 on transport failure. The ack write (small JSON object) only needs the
// status line back.
static int httpsPatch(const char *host, const String &path, const String &body,
                      String &respBody) {
  String req = String("PATCH ") + path + " HTTP/1.1\r\nHost: " + host +
               "\r\nContent-Type: application/json\r\nContent-Length: " +
               body.length() + "\r\nConnection: keep-alive\r\n\r\n";
  req += body;
  // The push reply echoes the whole node back (~1.3 KB and growing with the
  // event log), so this bound must clear it with margin: anything over the
  // cap DROPS the keep-alive, and at 1 push/s a drop-per-push is a permanent
  // handshake churn that fragments the heap until the board goes silent.
  return cloudRoundTrip(host, req, respBody, 4096);
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
  // loop tick (its own timer, so push and poll TLS never overlap - they
  // share one keep-alive session and loop() is single-threaded).
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

// Copies the live readings into the cloud payload. Runs UNDER dataMutex
// (taken here, 50 ms max) so the numbers are mutually consistent; the TLS
// that follows runs WITHOUT it.
//
// Shape is a superset of the local dashboard snapshot's Dashboard+History
// fields so the cloud viewer can render the SAME cards, totals and event
// log: per channel n/a/w/pf/kwh/s/mkwh, plus firmware version and the last
// 10 events. The RTDB .validate only requires dev/epoch/uptime/rssi/v/ch,
// so older rules still accept this - the extra keys are allowed.
bool CloudPush::snapshot(SystemData *sysData, SemaphoreHandle_t *mutex, String &body) {
  if (!sysData || !mutex) return false;
  bool locked = (xSemaphoreTake(*mutex, pdMS_TO_TICKS(50)) == pdTRUE);

  float v = sysData->voltageRMS;
  uint32_t up = sysData->uptime;
  int8_t rssi = sysData->wifiRSSI;
  float mcu = sysData->mcuTempC;
  bool eco = sysData->ecoMode;
  bool wifi = sysData->wifiConnected;
  bool ap = sysData->apMode;
  bool timeOk = timeSync.isSynced();
  long timeAge = (long)timeSync.secondsSinceSync();
  bool cloudEn = sysData->cloudEnabled;
  bool cloudOk = sysData->cloudOk;
  long cloudAge = (long)sysData->cloudAgeS;
  char nm[NUM_CHANNELS][MAX_CHANNEL_NAME_LEN];
  float a[NUM_CHANNELS], w[NUM_CHANNELS], pf[NUM_CHANNELS], kwh[NUM_CHANNELS],
      mkwh[NUM_CHANNELS];
  uint8_t st[NUM_CHANNELS];
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    strncpy(nm[ch], sysData->channels[ch].name, MAX_CHANNEL_NAME_LEN - 1);
    nm[ch][MAX_CHANNEL_NAME_LEN - 1] = '\0';
    a[ch] = sysData->channels[ch].currentRMS;
    w[ch] = sysData->channels[ch].activePower;
    pf[ch] = sysData->channels[ch].powerFactor;
    kwh[ch] = sysData->channels[ch].energyKWh;
    mkwh[ch] = sysData->channels[ch].monthlyKwhLimit;
    st[ch] = (uint8_t)sysData->channels[ch].status;
  }
  // Last 10 events, same window as the local snapshot (system_json.cpp).
  uint8_t evCount = sysData->eventCount;
  uint8_t evStart = evCount > 10 ? evCount - 10 : 0;
  uint8_t evN = evCount > evStart ? evCount - evStart : 0;
  uint32_t evT[10];
  uint8_t evC[10], evS[10];
  float evV[10];
  char evM[10][EVENT_MSG_LEN];
  for (uint8_t i = 0; i < evN; i++) {
    const Event &ev = sysData->events[evStart + i];
    evT[i] = ev.timestamp;
    evC[i] = ev.channel;
    evS[i] = (uint8_t)ev.status;
    evV[i] = ev.value;
    strncpy(evM[i], ev.message, EVENT_MSG_LEN - 1);
    evM[i][EVENT_MSG_LEN - 1] = '\0';
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
  body += ",\"wifi\":";
  body += wifi ? "true" : "false";
  body += ",\"ap\":";
  body += ap ? "true" : "false";
  body += ",\"fw\":\"";
  body += FIRMWARE_VERSION;
  body += "\",\"time\":{\"ok\":";
  body += timeOk ? "true" : "false";
  body += ",\"age\":";
  body += timeAge;
  body += "},\"cloud\":{\"en\":";
  body += cloudEn ? "true" : "false";
  body += ",\"ok\":";
  body += cloudOk ? "true" : "false";
  body += ",\"age\":";
  body += cloudAge;
  body += "},\"ch\":[";
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    body += "{\"n\":\"";
    body += cloudEscapeOut(String(nm[ch]));
    body += "\",\"a\":";
    body += String(a[ch], 2);
    body += ",\"w\":";
    body += String(w[ch], 1);
    body += ",\"pf\":";
    body += String(pf[ch], 3);
    body += ",\"kwh\":";
    body += String(kwh[ch], 3);
    body += ",\"s\":";
    body += st[ch];
    body += ",\"mkwh\":";
    body += String(mkwh[ch], 1);
    body += "}";
    if (ch < NUM_CHANNELS - 1) body += ",";
  }
  body += "],\"events\":[";
  for (uint8_t i = 0; i < evN; i++) {
    body += "{\"t\":";
    body += evT[i];
    body += ",\"c\":";
    body += evC[i];
    body += ",\"s\":";
    body += evS[i];
    body += ",\"v\":";
    body += String(evV[i], 1);
    body += ",\"m\":\"";
    body += cloudEscapeOut(String(evM[i]));
    body += "\"}";
    if (i + 1 < evN) body += ",";
  }
  body += "]}";
  return true;
}

// One PATCH, one fresh connection, no reads beyond the status line. The ID
// token travels inside the TLS tunnel; it is never logged, never stored
// anywhere but RAM, and never rendered. Returns the HTTP status so the caller
// can tell 401 (re-login) from failure (log and forget).
int CloudPush::postStatus(const String &body) {
  String req = "PATCH /devices/";
  req += deviceId_;
  req += "/latest.json?auth=";
  req += idToken_;
  req += " HTTP/1.1\r\nHost: ";
  req += host_;
  req += "\r\nContent-Type: application/json\r\nContent-Length: ";
  req += body.length();
  req += "\r\nConnection: keep-alive\r\n\r\n";
  req += body;

  String resp;
  int code = cloudRoundTrip(host_, req, resp, 1024);
  if (code == 0) {
    DEBUG_LOG("  [CLOUD] push failed (%s)\n", host_);
  }
  return code;
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
  // Downlink on its own timer so a slow push round trip never starves a poll
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
  consoleAppendf(out, "  %-10s%s (%s)", "Conn:",
                 s_connOpen ? "hot (keep-alive)" : "cold",
                 s_connOpen ? s_connHost : "next call dials");
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

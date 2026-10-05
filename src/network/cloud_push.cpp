#include "cloud_push.h"

#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>  // tokenStatusCallback: logs token lifecycle,
                                 // never the token itself
#include <addons/RTDBHelper.h>
#include <time.h>

#include "../utils/nvs_manager.h"
#include "../utils/log_gate.h"
#include "time_sync.h"
#include "command_processor.h"  // downlink executes cloud frames as commands
#include "console_handler.h"  // consoleAppendf: diag() reports through it

// Firebase SDK transport (the August smooth era, minus its one sin).
//
// The hand-rolled keep-alive is gone: every HTTPS call below goes through
// Firebase_ESP_Client 4.x, which owns the TLS session, the ID-token refresh
// and the retry the way a tested library does instead of ~200 lines of
// hand framing. Auth is herd email/password (auth.user.*), NOT the old
// service-account key: no private key lives on this board, and a flash dump
// yields at most one herd account's password, never project admin keys.
// Two FirebaseData objects = two dedicated sessions (push vs poll), exactly
// the "never share the hot path" rule in lessons.md ("Archived - cloud
// era"). Buffers match the proven August values: the node echoes back
// ~1.3 KB, so 4096 in / 1024 out with margin.
static FirebaseData s_pushFbdo;
static FirebaseData s_pollFbdo;
static FirebaseAuth s_auth;
static FirebaseConfig s_cfg;
static bool s_sdkStarted = false;
static char s_sdkEmail[CLOUD_MAX_EMAIL_LEN + 1] = {0};

// Starts the SDK once per boot (setcloud/clearcloud reboot by design, so
// credentials cannot change under a live session). After WiFi only:
// Firebase.begin() with no link just fails.
static void cloudSdkEnsure(const char *host, const char *email,
                           const char *pass) {
  if (s_sdkStarted) return;
  s_cfg.api_key = CLOUD_API_KEY_DEFAULT;
  String url = "https://";
  url += host;
  url += "/";
  // database_url must outlive begin(): the SDK keeps the pointer, so hold
  // it in a static, not a local.
  static char s_dbUrl[192] = {0};
  strncpy(s_dbUrl, url.c_str(), sizeof(s_dbUrl) - 1);
  s_dbUrl[sizeof(s_dbUrl) - 1] = '\0';
  s_cfg.database_url = s_dbUrl;
  strncpy(s_sdkEmail, email, sizeof(s_sdkEmail) - 1);
  s_sdkEmail[sizeof(s_sdkEmail) - 1] = '\0';
  s_auth.user.email = s_sdkEmail;
  static char s_sdkPass[CLOUD_MAX_PASS_LEN + 1] = {0};
  strncpy(s_sdkPass, pass, sizeof(s_sdkPass) - 1);
  s_sdkPass[sizeof(s_sdkPass) - 1] = '\0';
  s_auth.user.password = s_sdkPass;
  s_pushFbdo.setBSSLBufferSize(4096, 1024);
  s_pushFbdo.setResponseSize(4096);
  s_pollFbdo.setBSSLBufferSize(4096, 1024);
  s_pollFbdo.setResponseSize(4096);
  s_cfg.token_status_callback = tokenStatusCallback;
  Firebase.reconnectNetwork(false);  // WiFi belongs to WiFiManager.
  Firebase.begin(&s_cfg, &s_auth);
  s_sdkStarted = true;
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
  // loop tick (its own timer on its own SDK session, so push and poll never
  // share timing).
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

// Auth is the SDK's job now: email/password sign-in with the herd account,
// ID-token refresh handled internally (same model as the REST era, minus the
// hand-rolled JWT parsing that could wedge). ensureLogin() only makes sure
// the SDK was started (after WiFi) and reports readiness; a false return is
// "not authed yet", never a password problem (passwords are never logged).
bool CloudPush::ensureLogin() {
  cloudSdkEnsure(host_, email_, pass_);
  if (!Firebase.ready()) {
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 30000) {
      lastLog = millis();
      DEBUG_LOG("  [CLOUD] sdk not ready yet (auth in progress)\n");
    }
    return false;
  }
  return true;
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

// One PATCH via the SDK (updateNode = merge, the REST PATCH equivalent).
// The SDK attaches the ID token itself; it is never logged, never stored
// anywhere but the SDK's RAM session, and never rendered. Returns the HTTP
// status so the caller can tell auth problems from transport failure.
int CloudPush::postStatus(const String &body) {
  String path = "/devices/";
  path += deviceId_;
  path += "/latest";
  FirebaseJson payload;
  payload.setJsonData(body);
  if (Firebase.RTDB.updateNode(&s_pushFbdo, path, &payload)) {
    return 200;
  }
  DEBUG_LOG("  [CLOUD] push failed: %s\n", s_pushFbdo.errorReason().c_str());
  return 0;
}

bool CloudPush::post(const String &body) {
  int code = postStatus(body);
  if (code != 200) {
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
  if (!ensureLogin()) return;  // SDK auth in progress; never loops.

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
  getPath += "/cmd";
  String cmdBody;
  if (!Firebase.RTDB.getJSON(&s_pollFbdo, getPath)) {
    DEBUG_LOG("  [CLOUD] cmd poll failed: %s\n",
              s_pollFbdo.errorReason().c_str());
    return;
  }
  cmdBody = s_pollFbdo.jsonString();
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
  ackPath += "/ack";
  FirebaseJson ackJson;
  ackJson.setJsonData(ackBody);
  if (!Firebase.RTDB.setJSON(&s_pollFbdo, ackPath, &ackJson)) {
    DEBUG_LOG("  [CLOUD] cmd ack failed: %s\n",
              s_pollFbdo.errorReason().c_str());
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
      // Heap floor: the hand-rolled 1 s death was heap fragmentation from
      // per-push handshake churn. The SDK owns its session now, but the floor
      // stays: a 2 s reboot outage beats a silent hang that needs a human
      // with a USB cable.
      if (ESP.getFreeHeap() < 30000) {
        DEBUG_LOG("  [CLOUD] heap %lu - reboot\n", (unsigned long)ESP.getFreeHeap());
        delay(100);
        ESP.restart();
      }
      if (!ensureLogin()) { lastOk_ = false; consecFails_++; }
      else {
        String body;
        if (snapshot(sysData, mutex, body)) {
          if (post(body)) {
            lastOk_ = true;
            lastOkMs_ = now;
            consecFails_ = 0;
          } else {
            lastOk_ = false;
            consecFails_++;
          }
        }
      }
      // 60 straight failures (~60 s at 1 s cadence) with the link up means
      // the session is wedged, not the network. Reboot; the counters and
      // creds are in NVS, so nothing is lost.
      if (consecFails_ >= 60) {
        DEBUG_LOG("  [CLOUD] %lu straight fails - reboot\n", (unsigned long)consecFails_);
        delay(100);
        ESP.restart();
      }
    }
  }
  // Downlink on its own 2 s timer on its own FirebaseData session, so a slow
  // push never starves a poll (and vice versa). The STA + login gate lives
  // inside pollCmd.
  if (now - lastPollMs_ >= POLL_INTERVAL_MS) {
    lastPollMs_ = now;
    pollCmd(sysData, mutex, powerCalc, limitMgr, nvs);
  }
}

void CloudPush::diag(String &out, SemaphoreHandle_t *mutex) {
  // Snapshot the command id under the mutex: the WS console runs on a
  // different task than loop(), and a torn read here would report a
  // "mangled" id that is really just bytes mid-write.
  char cmd[sizeof(lastCmdId_)];
  cmd[0] = '\0';
  bool locked = (mutex && xSemaphoreTake(*mutex, pdMS_TO_TICKS(50)) == pdTRUE);
  if (lastCmdId_[0] != '\0') memcpy(cmd, lastCmdId_, sizeof(cmd));
  if (locked) xSemaphoreGive(*mutex);
  // Single-cycle aligned read like the push age above it.
  uint32_t pollAgeS = secondsSincePoll();

  consoleAppendf(out, "  %-10s%s", "Enabled:", enabled_ ? "yes" : "no");
  consoleAppendf(out, "  %-10s%s", "Session:",
                 Firebase.ready() ? "sdk ready (token managed internally)"
                                   : "auth in progress");
  consoleAppendf(out, "  %-10s%lu straight fails / heap %lu", "Health:",
                 (unsigned long)consecFails_, (unsigned long)ESP.getFreeHeap());
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
  // There is deliberately no token or password printable here: the SDK holds
  // the ID token internally and this function has no accessor for it. A
  // check_docs rule asserts that property against the source.
}

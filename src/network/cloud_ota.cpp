#include "cloud_ota.h"

CloudOTA cloudOta;

CloudOTA::CloudOTA()
  : inProgress(false), progress(0), state("idle"), hasPending(false) {}

void CloudOTA::begin() {
  Preferences prefs;
  prefs.begin("ota", false);
  appliedMd5 = prefs.getString("appliedMd5", "");
  prefs.end();
  if (appliedMd5.length()) {
    Serial.printf("  CloudOTA: applied md5=%s\n", appliedMd5.c_str());
  }
}

void CloudOTA::trigger(const String &version, const String &url, const String &md5) {
  if (inProgress) return;
  pendingVersion = version;
  pendingUrl = url;
  pendingMd5 = md5;
  hasPending = true;
}

void CloudOTA::loop() {
  if (inProgress) return;
  if (!hasPending) return;
  hasPending = false;
  runUpdate();
}

void CloudOTA::runUpdate() {
  inProgress = true;
  state = "downloading";
  progress = 0;

  Serial.printf("\n  [OTA] %s -> %s started\n", FIRMWARE_VERSION, pendingVersion.c_str());

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setReuse(false);
  http.addHeader("Connection", "close");

  if (!http.begin(client, pendingUrl)) {
    Serial.println("  [OTA] http.begin failed");
    state = "failed:begin";
    inProgress = false;
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("  [OTA] GET failed: HTTP %d\n", code);
    http.end();
    state = "failed:http";
    inProgress = false;
    return;
  }

  int total = http.getSize();
  if (total <= 0) {
    http.end();
    state = "failed:no-size";
    inProgress = false;
    return;
  }

  Serial.printf("  [OTA] downloading %d bytes\n", total);

  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("  [OTA] Update.begin failed: %s\n", Update.errorString());
    http.end();
    state = "failed:flash";
    inProgress = false;
    return;
  }

  Stream *s = http.getStreamPtr();
  s->setTimeout(5000);

  MD5Builder hasher;
  hasher.begin();

  uint8_t buf[4096];
  size_t done = 0;
  uint16_t stalls = 0;
  bool streamOk = true;

  while (done < (size_t)total) {
    size_t n = s->readBytes(buf, sizeof(buf));
    if (n == 0) {
      if (++stalls > 200) { streamOk = false; break; }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    stalls = 0;
    if (Update.write(buf, n) != n) {
      Serial.printf("  [OTA] Update.write failed at %u/%u\n", done, total);
      streamOk = false;
      break;
    }
    hasher.add(buf, n);
    done += n;

    progress = (uint8_t)((done * 100) / total);

    // Yield to ALL tasks including IDLE (feeds the WDT on Core 0).
    // vTaskDelay blocks this task for ~1 tick, letting IDLE run.
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  http.end();

  if (!streamOk || done != (size_t)total) {
    Update.abort();
    state = "failed:stream";
    inProgress = false;
    return;
  }

  hasher.calculate();
  String got = hasher.toString();
  Serial.printf("  [OTA] download complete, md5=%s\n", got.c_str());

  if (!pendingMd5.isEmpty() && !got.equalsIgnoreCase(pendingMd5)) {
    Serial.printf("  [OTA] md5 mismatch: got %s, expected %s\n", got.c_str(), pendingMd5.c_str());
    Update.abort();
    state = "failed:md5";
    inProgress = false;
    return;
  }

  if (!Update.end()) {
    Serial.printf("  [OTA] Update.end failed: %s\n", Update.errorString());
    state = "failed:flash";
    inProgress = false;
    return;
  }

  // Remember which image is now flashed.
  Preferences prefs;
  prefs.begin("ota", false);
  prefs.putString("appliedMd5", got);
  prefs.end();
  appliedMd5 = got;

  state = "done";
  progress = 100;
  inProgress = false;

  Serial.printf("\n  [OTA] %s applied. Rebooting.\n", pendingVersion.c_str());
  delay(1500);
  ESP.restart();
}

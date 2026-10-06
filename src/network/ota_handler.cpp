#include "ota_handler.h"
#include "ota_url.h"
#include "cloud_push.h"
#include "../utils/nvs_manager.h"

#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

void OTAHandler::begin(const char *hostname) {
  inProgress = false;
  progress = 0;
  cloudBusy = false;
  cloudActive = false;
  rebootDue = false;
  cloudProgress = 0;
  cloudUrl[0] = '\0';
  cloudErr[0] = '\0';

  ArduinoOTA.setHostname(hostname);

  ArduinoOTA.onStart([this]() {
    inProgress = true;
    progress = 0;
  });

  ArduinoOTA.onProgress([this](unsigned int bytes, unsigned int total) {
    progress = (bytes * 100) / total;
  });

  ArduinoOTA.onEnd([this]() {
    inProgress = false;
    progress = 100;
  });

  ArduinoOTA.onError([this](ota_error_t error) {
    inProgress = false;
  });

  ArduinoOTA.begin();
}

void OTAHandler::loop() {
  ArduinoOTA.handle();
  // First tick with a home link consumes a staged link: this runs BEFORE the
  // cloud SDK ever starts (push runs later on this same tick), so the
  // download below handshakes against a boot-fresh heap. The CLOCK is not
  // required here - loopCloud waits for it inside the updater, still before
  // anything else can run. No stall when STA is down: the stage simply waits
  // for a later tick.
  if (!cloudBusy && !cloudActive && !havePending_ && nvs_) {
    havePending_ = nvs_->loadOtaPending(pendUrl_, sizeof(pendUrl_));
  }
  if (!cloudBusy && !cloudActive && havePending_) {
    if (WiFi.status() == WL_CONNECTED) {
      strncpy(cloudUrl, pendUrl_, sizeof(cloudUrl) - 1);
      cloudUrl[sizeof(cloudUrl) - 1] = '\0';
      havePending_ = false;
      cloudErr[0] = '\0';
      errSaved_ = false;
      cloudProgress = 0;
      if (nvs_) {
        bool locked = (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) == pdTRUE);
        nvs_->clearOtaPending();
        nvs_->clearOtaErr();
        nvs_->commit();
        if (locked) xSemaphoreGive(mutex_);
      }
      cloudBusy = true;
    }
  }
  if (cloudBusy) {
    loopCloud();
    // One NVS failure record per attempt: the updater reboot would otherwise
    // erase why it failed. Committed under the mutex (sensorTask shares the
    // handle); a later stage or a verified image clears it.
    if (!cloudBusy && !cloudActive && !rebootDue && cloudErr[0] && !errSaved_) {
      errSaved_ = true;
      if (nvs_) {
        bool locked = (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) == pdTRUE);
        nvs_->saveOtaErr(cloudErr);
        nvs_->commit();
        if (locked) xSemaphoreGive(mutex_);
      }
    }
  }
  // Auto-poll runs on the same tick (notify-only, never stages). It skips
  // itself while a download is staged or running - see pollTick.
  pollTick();
}

bool OTAHandler::startCloudUpdate(const char *url, bool staUp, String &reply) {
  if (inProgress || cloudBusy) {
    reply = "  An update is already running - wait for it to finish.";
    return false;
  }
  if (!staUp) {
    reply = "  No home network: cloud OTA needs STA (the fallback AP has no internet).";
    return false;
  }
  const char *why = nullptr;
  if (!url || !ota_url_validate(url, &why)) {
    reply = String("  Not started: ") + (why ? why : "bad URL.");
    return false;
  }
  // Staged, not downloaded: the caller persists the link and reboots (see
  // cmdOta). The updater tick above does the download pre-SDK, so the reply
  // promises the reboot + banner, never live progress - snapshots stall
  // during the blocking fetch, so percent cannot stream.
  const char *base = strrchr(url, '/');
  reply = String("  Staged \"") + (base ? base + 1 : url) +
          "\" - rebooting into the updater, banner above while it downloads, `ota status` for detail.";
  return true;
}

int OTAHandler::compareVersions(const String &a, const String &b) {
  // cloud-ota compareVersion verbatim: dotted-decimal, missing parts read
  // as 0, so 1.10.0 beats 1.9.9 and "3.2" equals "3.2.0".
  String aa = a, bb = b;
  aa.trim();
  bb.trim();
  int ai = 0, bi = 0, apos = 0, bpos = 0;
  while (apos < (int)aa.length() || bpos < (int)bb.length()) {
    int aEnd = aa.indexOf('.', apos);
    int bEnd = bb.indexOf('.', bpos);
    if (aEnd == -1) aEnd = aa.length();
    if (bEnd == -1) bEnd = bb.length();
    ai = aa.substring(apos, aEnd).toInt();
    bi = bb.substring(bpos, bEnd).toInt();
    if (ai < bi) return -1;
    if (ai > bi) return 1;
    apos = aEnd + 1;
    bpos = bEnd + 1;
    if (apos > (int)aa.length()) apos = aa.length();
    if (bpos > (int)bb.length()) bpos = bb.length();
  }
  return 0;
}

void OTAHandler::prepareTlsWindow() {
  if (cloud_) cloud_->releaseSessions();
  vTaskDelay(pdMS_TO_TICKS(200));  // let LWIP release the closed PCBs
}

bool OTAHandler::fetchLatest(String &latest, String &binUrl, String *detail) {
  // Small GET only: a ~200-byte JSON fetch. The CALLER frees the SDK
  // sessions first (see prepareTlsWindow) - with them up the max block
  // sits at ~34 KB and no handshake completes. The firmware bytes still
  // flow in the pre-SDK updater tick (see loopCloud). `detail` carries the
  // short failure reason for the `update` verb; the auto-poll passes null
  // and stays silent.
  latest = "";
  binUrl = "";
  WiFiClientSecure client;
  if (OTA_USE_INSECURE) {
    client.setInsecure();
  } else {
    client.useBuiltinCACertBundle();
  }
  client.setTimeout(15000);
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  String url = String(OTA_VERSION_URL) + "?t=" + String(millis());
  // Stage the failure first: HTTP -1 means "never connected", which
  // conflates DNS, routing and TLS - so probe plain TCP first (cheap, no
  // TLS): if it fails the router/DNS is the problem; if it passes but
  // HTTPS fails, it is TLS (heap). Same split the updater tick uses.
  {
    String host = String(OTA_VERSION_URL);
    host.replace("https://", "");
    host.replace("http://", "");
    int slash = host.indexOf('/');
    if (slash > 0) host = host.substring(0, slash);
    NetworkClient probe;
    probe.setTimeout(5000);
    if (!probe.connect(host.c_str(), 443)) {
      if (detail) *detail = String("no route to ") + host + ":443 (DNS/router?).";
      return false;
    }
    probe.stop();
  }
  if (!http.begin(client, url)) {
    if (detail) *detail = "HTTP setup failed (out of memory?).";
    return false;
  }
  http.addHeader("Cache-Control", "no-cache");
  http.addHeader("Pragma", "no-cache");
  int code = http.GET();
  if (code < 0) {
    if (detail) {
      char buf[96];
      snprintf(buf, sizeof(buf), "TLS failed (heap %lu/max %lu).",
               (unsigned long)ESP.getFreeHeap(),
               (unsigned long)ESP.getMaxAllocHeap());
      *detail = buf;
    }
    http.end();
    return false;
  }
  if (code != 200) {
    if (detail) *detail = String("version.json GET ") + code + ".";
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    if (detail) *detail = "version.json did not parse.";
    return false;
  }
  String v = doc["version"] | "";
  // Per-chip asset (counter releases ship two .bins); legacy single
  // bin_url kept as fallback so a cloud-ota-shaped file still works.
  String legacy = doc["bin_url"] | "";
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  String b = doc["s3_bin_url"] | legacy;
#else
  String b = doc["classic_bin_url"] | legacy;
#endif
  v.trim();
  b.trim();
  if (v == "" || b == "") {
    if (detail) *detail = "version.json missing version/bin URL.";
    return false;
  }
  latest = v;
  binUrl = b;
  return true;
}

void OTAHandler::pollTick() {
  // Notify-only: never stage, never reboot. Skips while the updater owns
  // the radio path (staged link, active download, verified image waiting
  // for its deferred reboot, or the LAN updater running).
  if (inProgress || cloudBusy || cloudActive || havePending_ || rebootDue) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;  // offline: retry next tick, timer untouched so reconnect checks soon
  }
  unsigned long now = millis();
  if (lastPollMs_ == 0) {
    // First boot: let STA/DHCP settle before the first fetch.
    if (now < 60000UL) {
      return;
    }
  } else if (now - lastPollMs_ < (unsigned long)OTA_CHECK_INTERVAL_MS) {
    return;
  }
  lastPollMs_ = now;
  prepareTlsWindow();
  String latest, binUrl;
  if (!fetchLatest(latest, binUrl)) {
    return;  // failed fetch stays silent; `update` reports it on demand
  }
  if (compareVersions(String(FIRMWARE_VERSION), latest) < 0) {
    const char *why = nullptr;
    if (!ota_url_validate(binUrl.c_str(), &why)) {
      return;  // bad link stays silent here; `update` names it
    }
    updateAvailable_ = true;
    strncpy(latestVer_, latest.c_str(), sizeof(latestVer_) - 1);
    latestVer_[sizeof(latestVer_) - 1] = '\0';
    // Anti-spam latch: one serial line per release per boot.
    if (strcmp(noticedVer_, latest.c_str()) != 0) {
      strncpy(noticedVer_, latest.c_str(), sizeof(noticedVer_) - 1);
      noticedVer_[sizeof(noticedVer_) - 1] = '\0';
      Serial.print("  NEW VERSION! ");
      Serial.print(FIRMWARE_VERSION);
      Serial.print(" -> ");
      Serial.print(latest);
      Serial.println(" - type `update` to flash.");
    }
  } else {
    // Already on latest (or newer than the manifest, e.g. a dev build):
    // clear any stale notice and stay silent.
    updateAvailable_ = false;
    latestVer_[0] = '\0';
    noticedVer_[0] = '\0';
  }
}

bool OTAHandler::checkForUpdate(bool doInstall, bool staUp, bool verbose,
                                String &reply, String &stageUrl) {
  stageUrl = "";
  if (inProgress || cloudBusy || cloudActive) {
    reply = "  An update is already running - wait for it to finish.";
    return false;
  }
  if (!staUp) {
    reply = "  No home network: cloud OTA needs STA (the fallback AP has no internet).";
    return false;
  }
  prepareTlsWindow();
  String latest, binUrl, why_not;
  if (!fetchLatest(latest, binUrl, &why_not)) {
    reply = String("  Check failed: ") + why_not;
    return false;
  }
  int cmp = compareVersions(String(FIRMWARE_VERSION), latest);
  if (cmp >= 0) {
    reply = String("  Already on latest (") + FIRMWARE_VERSION + " >= " +
            latest + ").";
    if (verbose) reply += " Checked " + String(OTA_VERSION_URL) + ".";
    return false;
  }
  const char *why = nullptr;
  if (!ota_url_validate(binUrl.c_str(), &why)) {
    reply = String("  New version ") + latest +
            " found, but its link is bad: " + (why ? why : "bad URL.");
    return false;
  }
  if (!doInstall) {
    reply = String("  NEW VERSION! ") + FIRMWARE_VERSION + " -> " + latest +
            " - type `update` to flash.";
    return false;
  }
  const char *base = strrchr(binUrl.c_str(), '/');
  reply = String("  NEW VERSION ") + FIRMWARE_VERSION + " -> " + latest +
          ": staged \"" + (base ? base + 1 : binUrl.c_str()) +
          "\" - rebooting into the updater, banner above while it downloads, `ota status` for detail.";
  stageUrl = binUrl;
  return true;
}

// Downloads the armed URL into the inactive OTA slot, then raises
// rebootDue. Runs on the network task: every chunk yields (vTaskDelay) so
// the IDLE task still feeds the task watchdog across a ~60 s download, and
// the sensor task on Core 1 never notices. Blocking here stalls WS
// broadcasts and the 1 s cloud push for the duration - the same trade the
// REST push already makes per round trip, and the dashboard shows STALE +
// the OTA banner meanwhile, which is the honest display.
void OTAHandler::loopCloud() {
  cloudBusy = false;  // one attempt per arm; startCloudUpdate re-arms
  cloudProgress = 0;
  cloudActive = true;  // banner + LED for the whole fetch, cleared on exit

  // 3.2.6 proved a 34 KB largest block cannot finish even an INSECURE
  // handshake, and the two Firebase keep-alive sessions are the biggest
  // contiguous holders on a cloud-enabled board. Drop them first so the
  // download below handshakes against a clean heap; the SDK reconnects on
  // next use (failed download resumes pushing by itself, verified one
  // reboots anyway). Same-task blocking means nothing can re-establish
  // mid-download: push/poll run after this on the same networkTask tick.
  if (cloud_) cloud_->releaseSessions();
  vTaskDelay(pdMS_TO_TICKS(200));  // let LWIP release the closed PCBs

  // Insecure transport needs no clock (no chain to validate), so unlike
  // the 3.2.x validated downloader there is no NTP wait here - the cloud-ota
  // rig flashes with whatever clock it has. Only the link matters.
  if (WiFi.status() != WL_CONNECTED) {
    strncpy(cloudErr, "STA dropped before the download began.", sizeof(cloudErr) - 1);
    cloudActive = false;
    return;
  }

  // Stage the failure, don't just print a number. HTTP -1 means "never
  // connected", which conflates DNS, routing and TLS - so probe plain TCP
  // first (cheap, no TLS): if it fails the router/DNS is the problem; if it
  // passes but HTTPS fails, it is TLS (heap). The validator only ever arms
  // github.com links, so the probe target is fixed.
  uint32_t heap0 = ESP.getFreeHeap();
  {
    NetworkClient probe;
    probe.setTimeout(5000);
    if (!probe.connect("github.com", 443)) {
      snprintf(cloudErr, sizeof(cloudErr),
               "no route to github.com:443 (DNS/router?).");
      cloudActive = false;
      return;
    }
    probe.stop();
  }

  // cloud-ota proven transport: HTTPUpdate over an insecure client, same as
  // the rig that flashes reliably. No chain validation (see config.h note);
  // the image itself is still validated by Update.end() inside HTTPUpdate,
  // and a bad write keeps the old firmware. rebootOnUpdate(false) so the
  // sketch reboots through the deferred path and the counters flush first.
  WiFiClientSecure otaClient;
  if (OTA_USE_INSECURE) {
    otaClient.setInsecure();
  } else {
    otaClient.useBuiltinCACertBundle();
  }
  otaClient.setTimeout(30000);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // github.com -> CDN
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.onProgress([this](size_t cur, size_t total) {
    if (total > 0) cloudProgress = (uint8_t)((cur * 100) / total);
  });
  cloudProgress = 0;
  t_httpUpdate_return ret = httpUpdate.update(otaClient, String(cloudUrl));
  switch (ret) {
    case HTTP_UPDATE_OK:
      cloudProgress = 100;
      cloudActive = false;  // banner's job is done; the reboot line takes over
      rebootDue = true;  // the sketch reboots via the deferred path (NVS flush)
      return;
    case HTTP_UPDATE_NO_UPDATES:
      strncpy(cloudErr, "board reports no updates.", sizeof(cloudErr) - 1);
      break;
    case HTTP_UPDATE_FAILED:
    default:
      snprintf(cloudErr, sizeof(cloudErr), "FAILED err %d: %s (heap %lu).",
               (int)httpUpdate.getLastError(),
               httpUpdate.getLastErrorString().c_str(),
               (unsigned long)heap0);
      break;
  }
  cloudActive = false;
}

void OTAHandler::cloudStatus(String &out) {
  char line[160];
  if (cloudBusy || cloudActive) {
    snprintf(line, sizeof(line), "  OTA: downloading %u%%", (unsigned)cloudProgress);
    out += line;
  } else if (rebootDue) {
    out += "  OTA: verified, rebooting into the new firmware...";
  } else if (cloudErr[0]) {
    snprintf(line, sizeof(line), "  OTA: last download failed: %s", cloudErr);
    out += line;
  } else if (havePending_) {
    out += "  OTA: staged, waiting for a home link (fallback AP has no internet).";
  } else if (updateAvailable_) {
    snprintf(line, sizeof(line),
             "  NEW VERSION! %s -> %s - type `update` to flash.",
             FIRMWARE_VERSION, latestVer_);
    out += line;
  } else if (nvs_) {
    char e[128];
    if (nvs_->loadOtaErr(e, sizeof(e))) {
      snprintf(line, sizeof(line), "  OTA: last updater run failed: %s", e);
      out += line;
    } else {
      out += "  OTA: idle (no cloud download staged or run).";
    }
  } else {
    out += "  OTA: idle (no cloud download since boot).";
  }
}

#include "ota_handler.h"
#include "ota_url.h"
#include "cloud_push.h"
#include "../utils/nvs_manager.h"

#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <WiFi.h>
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
  // First tick with a home link + valid clock consumes a staged link: this
  // runs BEFORE the cloud SDK ever starts (push runs later on this same
  // tick), so the download below handshakes against a clean heap. No stall
  // when the clock is stale - the stage simply waits for a later tick.
  if (!cloudBusy && !cloudActive && !havePending_ && nvs_) {
    havePending_ = nvs_->loadOtaPending(pendUrl_, sizeof(pendUrl_));
  }
  if (!cloudBusy && !cloudActive && havePending_) {
    if (WiFi.status() == WL_CONNECTED && time(nullptr) > 1700000000L) {
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

  if (WiFi.status() != WL_CONNECTED) {
    strncpy(cloudErr, "STA dropped before the download began.", sizeof(cloudErr) - 1);
    cloudActive = false;
    return;
  }

  // Stage the failure, don't just print a number. HTTP -1 means "never
  // connected", which conflates DNS, routing and TLS - so probe plain TCP
  // first (cheap, no TLS): if it fails the router/DNS is the problem; if it
  // passes but HTTPS fails, it is TLS (clock or heap). The validator only
  // ever arms github.com links, so the probe target is fixed.
  uint32_t heap0 = ESP.getFreeHeap();
  bool clockOk = time(nullptr) > 1700000000L;
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

  // Raw-TLS probe: handshake WITHOUT validation, then stop. Sends no HTTP
  // and moves no firmware bytes - it only splits "TLS cannot handshake here
  // at all (memory/protocol)" from "handshake works, validation rejects the
  // chain (bundle/root)". The real download below always uses `client`.
  bool rawTlsOk = false;
  {
    NetworkClientSecure probe;
    probe.setInsecure();
    probe.setTimeout(8000);
    rawTlsOk = probe.connect("github.com", 443);
    probe.stop();
  }

  NetworkClientSecure client;
  client.useBuiltinCACertBundle();  // full chain validation, no PEM to maintain
  client.setTimeout(30000);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // github.com -> CDN
  http.setRedirectLimit(5);
  http.setTimeout(30000);
  if (!http.begin(client, cloudUrl)) {
    strncpy(cloudErr, "HTTP setup failed (out of memory?).", sizeof(cloudErr) - 1);
    cloudActive = false;
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      snprintf(cloudErr, sizeof(cloudErr),
               "TLS failed (heap %lu/max %lu, clock %s, raw-TLS %s).",
               (unsigned long)heap0, (unsigned long)ESP.getMaxAllocHeap(),
               clockOk ? "ok" : "STALE?", rawTlsOk ? "ok" : "NO");
    } else {
      snprintf(cloudErr, sizeof(cloudErr), "download refused: HTTP %d.", code);
    }
    http.end();
    cloudActive = false;
    return;
  }
  int total = http.getSize();
  if (total <= 0) {
    strncpy(cloudErr, "download has no known size - refusing.", sizeof(cloudErr) - 1);
    http.end();
    cloudActive = false;
    return;
  }
  if ((size_t)total > ESP.getFreeSketchSpace()) {
    snprintf(cloudErr, sizeof(cloudErr), "image %d B does not fit the %u B slot.",
             total, (unsigned)ESP.getFreeSketchSpace());
    http.end();
    cloudActive = false;
    return;
  }

  Update.onProgress([this](size_t done, size_t all) {
    if (all > 0) cloudProgress = (uint8_t)((done * 100) / all);
  });
  if (!Update.begin((size_t)total)) {
    snprintf(cloudErr, sizeof(cloudErr), "Update.begin failed (err %d).",
             (int)Update.getError());
    http.end();
    cloudActive = false;
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t remaining = (size_t)total;
  bool ok = true;
  while (ok && remaining > 0 && http.connected()) {
    size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
    int n = stream->readBytes(buf, want);
    if (n <= 0) { ok = false; break; }
    if (Update.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
    remaining -= (size_t)n;
    cloudProgress = (uint8_t)(((total - (int)remaining) * 100) / total);
    vTaskDelay(1);  // let IDLE run: a minute-long tight loop trips the WDT
  }
  http.end();

  if (!ok || remaining != 0) {
    snprintf(cloudErr, sizeof(cloudErr), "download stalled (%u B short).",
             (unsigned)remaining);
    Update.abort();
    cloudActive = false;
    return;
  }
  if (!Update.end(true)) {
    snprintf(cloudErr, sizeof(cloudErr), "image invalid (err %d) - old firmware kept.",
             (int)Update.getError());
    cloudActive = false;
    return;
  }

  cloudProgress = 100;
  cloudActive = false;  // banner's job is done; the reboot line takes over
  rebootDue = true;  // the sketch reboots via the deferred path (NVS flush)
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

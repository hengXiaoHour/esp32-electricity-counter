#include "ota_handler.h"
#include "ota_url.h"

#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <WiFi.h>

void OTAHandler::begin(const char *hostname) {
  inProgress = false;
  progress = 0;
  cloudBusy = false;
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
  if (cloudBusy) loopCloud();
}

void OTAHandler::startCloudUpdate(const char *url, bool staUp, String &reply) {
  if (inProgress || cloudBusy) {
    reply = "  An update is already running - wait for it to finish.";
    return;
  }
  if (!staUp) {
    reply = "  No home network: cloud OTA needs STA (the fallback AP has no internet).";
    return;
  }
  const char *why = nullptr;
  if (!url || !ota_url_validate(url, &why)) {
    reply = String("  Not started: ") + (why ? why : "bad URL.");
    return;
  }
  strncpy(cloudUrl, url, sizeof(cloudUrl) - 1);
  cloudUrl[sizeof(cloudUrl) - 1] = '\0';
  cloudErr[0] = '\0';
  cloudProgress = 0;
  cloudBusy = true;
  const char *base = strrchr(cloudUrl, '/');
  reply = String("  OTA started from \"") + (base ? base + 1 : cloudUrl) +
          "\" - progress above, `ota status` for detail.";
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

  if (WiFi.status() != WL_CONNECTED) {
    strncpy(cloudErr, "STA dropped before the download began.", sizeof(cloudErr) - 1);
    return;
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
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(cloudErr, sizeof(cloudErr), "download refused: HTTP %d.", code);
    http.end();
    return;
  }
  int total = http.getSize();
  if (total <= 0) {
    strncpy(cloudErr, "download has no known size - refusing.", sizeof(cloudErr) - 1);
    http.end();
    return;
  }
  if ((size_t)total > ESP.getFreeSketchSpace()) {
    snprintf(cloudErr, sizeof(cloudErr), "image %d B does not fit the %u B slot.",
             total, (unsigned)ESP.getFreeSketchSpace());
    http.end();
    return;
  }

  Update.onProgress([this](size_t done, size_t all) {
    if (all > 0) cloudProgress = (uint8_t)((done * 100) / all);
  });
  if (!Update.begin((size_t)total)) {
    snprintf(cloudErr, sizeof(cloudErr), "Update.begin failed (err %d).",
             (int)Update.getError());
    http.end();
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
    return;
  }
  if (!Update.end(true)) {
    snprintf(cloudErr, sizeof(cloudErr), "image invalid (err %d) - old firmware kept.",
             (int)Update.getError());
    return;
  }

  cloudProgress = 100;
  rebootDue = true;  // the sketch reboots via the deferred path (NVS flush)
}

void OTAHandler::cloudStatus(String &out) const {
  char line[160];
  if (cloudBusy) {
    snprintf(line, sizeof(line), "  OTA: downloading %u%%", (unsigned)cloudProgress);
    out += line;
  } else if (rebootDue) {
    out += "  OTA: verified, rebooting into the new firmware...";
  } else if (cloudErr[0]) {
    snprintf(line, sizeof(line), "  OTA: last download failed: %s", cloudErr);
    out += line;
  } else {
    out += "  OTA: idle (no cloud download yet this boot).";
  }
}

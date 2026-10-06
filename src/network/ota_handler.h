#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../config.h"
#include "ota_url.h"

class CloudPush;
class NVSManager;

// Local (LAN) ArduinoOTA plus cloud (GitHub release) updates, one progress
// signal for both: updateLED(), updateEcoMode() and the dashboard's OTA
// banner all read isInProgress()/getProgress(), so whichever transport is
// running shows the same way. Only one transport can run at a time.
//
// Cloud path (3.2.9 reboot-to-updater): `ota <url>` (serial / dashboard
// console / cloud console) only STAGES the link into NVS and reboots; the
// first network tick with STA + clock picks it up and downloads BEFORE the
// Firebase sessions exist, against a clean heap. 3.2.7 proved a 34 KB
// largest block cannot finish even an insecure handshake at runtime, and
// freed blocks scatter instead of coalescing - so no runtime download is
// attempted any more. On success the handler does NOT reboot itself - it
// raises cloudRebootDue(), and the sketch routes that through
// ConsoleHandler::requestReboot so the energy counters are flushed to NVS
// first (a reboot that loses a month of counters is not an update, it is
// data loss). On failure the board keeps running with the error readable
// via `ota status` (kept in NVS, so it survives the updater reboot too).
class OTAHandler {
public:
  void begin(const char *hostname = "esp32-elec-counter");
  void loop();

  bool isInProgress() const { return inProgress || cloudBusy || cloudActive; }
  uint8_t getProgress() const {
    return (cloudBusy || cloudActive) ? cloudProgress : progress;
  }

  // Validates `url` (github release .bin shape), refuses when the radio has
  // no STA link or another update is running. Returns true when the link
  // was staged: the CALLER persists it to NVS and reboots (see cmdOta).
  // `reply` is one short console line either way (it travels inside the
  // 200-char cloud ack, so it stays terse by construction).
  bool startCloudUpdate(const char *url, bool staUp, String &reply);

  // Store for the NVS staged link + last updater error (wired once at boot).
  // The mutex guards the two commits this makes per attempt (consume +
  // failure record) against sensorTask's energy save on the same handle.
  void setStore(NVSManager *n, SemaphoreHandle_t *m) {
    nvs_ = n;
    mutex_ = m;
  }

  // One-line state for `ota status` (serial + both dashboards' consoles).
  void cloudStatus(String &out) const;

  // Raised once when a download verified and is ready to boot. The sketch
  // consumes it into a deferred reboot; until then the board keeps running
  // the OLD firmware.
  bool cloudRebootDue() const { return rebootDue; }
  void consumeCloudReboot() { rebootDue = false; }

  // Wired once at boot (.ino): the download entry drops the SDK keep-alive
  // sessions through this before probing github.com (see loopCloud).
  void setCloudPush(CloudPush *c) { cloud_ = c; }

private:
  bool inProgress;
  uint8_t progress;

  // Cloud-download state. cloudBusy also drives isInProgress(), so the LED
  // blinks yellow and eco holds the radio awake for the whole download with
  // no extra wiring.
  bool cloudBusy = false;
  // Stays true for the WHOLE download (cloudBusy is cleared on entry to keep
  // one-attempt semantics). Without this the ota flag lives for one tick and
  // the dashboard banner can never show during a ~60 s fetch.
  bool cloudActive = false;
  bool rebootDue = false;
  uint8_t cloudProgress = 0;
  char cloudUrl[OTA_URL_MAX_LEN + 1] = {0};
  char cloudErr[128] = {0};
  CloudPush *cloud_ = nullptr;

  void loopCloud();
};

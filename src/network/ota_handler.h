#pragma once

#include <Arduino.h>
#include "../config.h"
#include "ota_url.h"

// Local (LAN) ArduinoOTA plus cloud (GitHub release) updates, one progress
// signal for both: updateLED(), updateEcoMode() and the dashboard's OTA
// banner all read isInProgress()/getProgress(), so whichever transport is
// running shows the same way. Only one transport can run at a time.
//
// Cloud path: `ota <url>` (serial / dashboard console / cloud console) only
// RECORDS the URL after validating it; the download itself runs from loop()
// on the network task, so the acknowledgement returns instantly and sensing
// on Core 1 never stalls. On success the handler does NOT reboot itself -
// it raises cloudRebootDue(), and the sketch routes that through
// ConsoleHandler::requestReboot so the energy counters are flushed to NVS
// first (a reboot that loses a month of counters is not an update, it is
// data loss). On failure the board stays running with the error readable
// via `ota status`.
class OTAHandler {
public:
  void begin(const char *hostname = "esp32-elec-counter");
  void loop();

  bool isInProgress() const { return inProgress || cloudBusy; }
  uint8_t getProgress() const { return cloudBusy ? cloudProgress : progress; }

  // Validates `url` (github release .bin shape), refuses when the radio has
  // no STA link or another update is running, otherwise arms the download.
  // `reply` is one short console line either way (it travels inside the
  // 200-char cloud ack, so it stays terse by construction).
  void startCloudUpdate(const char *url, bool staUp, String &reply);

  // One-line state for `ota status` (serial + both dashboards' consoles).
  void cloudStatus(String &out) const;

  // Raised once when a download verified and is ready to boot. The sketch
  // consumes it into a deferred reboot; until then the board keeps running
  // the OLD firmware.
  bool cloudRebootDue() const { return rebootDue; }
  void consumeCloudReboot() { rebootDue = false; }

private:
  bool inProgress;
  uint8_t progress;

  // Cloud-download state. cloudBusy also drives isInProgress(), so the LED
  // blinks yellow and eco holds the radio awake for the whole download with
  // no extra wiring.
  bool cloudBusy = false;
  bool rebootDue = false;
  uint8_t cloudProgress = 0;
  char cloudUrl[OTA_URL_MAX_LEN + 1] = {0};
  char cloudErr[96] = {0};

  void loopCloud();
};

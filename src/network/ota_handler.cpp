#include "ota_handler.h"

#include <ArduinoOTA.h>

void OTAHandler::begin(const char *hostname, NVSManager *nvsRef) {
  inProgress = false;
  progress = 0;
  nvs = nvsRef;

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
    // Mark the flashed-but-not-yet-verified state BEFORE the restart the
    // ArduinoOTA core triggers in its end handler. cleared on the first
    // successful network bring-up; if it is still set after a boot where
    // the network never came up, the last update did not verify.
    if (nvs) {
      nvs->saveOtaPending(true);
      nvs->commit();
    }
  });

  ArduinoOTA.onError([this](ota_error_t error) {
    (void)error;
    inProgress = false;
  });

  ArduinoOTA.begin();
}

void OTAHandler::loop() {
  ArduinoOTA.handle();
}

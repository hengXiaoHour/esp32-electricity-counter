#include "ota_handler.h"

#include <ArduinoOTA.h>

void OTAHandler::begin(const char *hostname) {
  inProgress = false;
  progress = 0;

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
    (void)error;
    inProgress = false;
  });

  ArduinoOTA.begin();
}

void OTAHandler::loop() {
  ArduinoOTA.handle();
}

#pragma once

#include <ArduinoOTA.h>
#include "../config.h"

class OTAHandler {
public:
  void begin(const char *hostname = "esp32-elec-counter");
  void loop();

  bool isInProgress() const { return inProgress; }
  uint8_t getProgress() const { return progress; }

private:
  bool inProgress;
  uint8_t progress;
};

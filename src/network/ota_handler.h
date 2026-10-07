#pragma once

#include <Arduino.h>
#include "../config.h"

// Local (LAN) ArduinoOTA only, under hostname `esp32-elec-counter`, begun
// from networkTask once the radio has an IP. There is no cloud updater: no
// github release download, no staged link, no `ota` console verb. Firmware
// updates happen over the local network (Arduino IDE network port / espota)
// or over USB. isInProgress()/getProgress() drive the LED blink, the eco
// hold and the dashboard's OTA banner while a LAN flash is running.
class OTAHandler {
public:
  void begin(const char *hostname = "esp32-elec-counter");
  void loop();

  bool isInProgress() const { return inProgress; }
  uint8_t getProgress() const { return progress; }

private:
  bool inProgress = false;
  uint8_t progress = 0;
};

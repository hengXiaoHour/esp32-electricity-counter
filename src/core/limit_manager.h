#pragma once

#include <Arduino.h>

#include "../config.h"
#include "../core/power_calculator.h"
#include "../network/ntfy_notifier.h"
#include "../ui/buzzer.h"
#include "../utils/nvs_manager.h"

class LimitManager {
public:
  void begin(NVSManager &nvs,
             PowerCalculator &powerCalc, SystemData *sysData,
             SemaphoreHandle_t *mutex, NtfyNotifier *ntfy, Buzzer *buzzer);

  // Call from sensorTask (~80ms). Takes dataMutex internally.
  void loop();

  // Manual counter reset (user-triggered). Zeroes the channel's energy.
  void resetCounter(uint8_t ch);

private:
  NVSManager *nvs;
  PowerCalculator *powerCalc;
  SystemData *sysData;
  SemaphoreHandle_t *dataMutex;
  NtfyNotifier *ntfy;
  Buzzer *buzzer;

  bool tripNotified[NUM_CHANNELS];
  uint8_t tripCycleIndex = 0;

  void logEvent(uint8_t ch, ChannelStatus s, const char *msg, float v);
  void rolloverIfNeeded();
  void checkLimits();
  void updateBuzzer();
};

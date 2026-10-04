#pragma once

#include <Arduino.h>

#include "../config.h"
#include "../core/power_calculator.h"
#include "../ui/buzzer.h"
#include "../utils/nvs_manager.h"

class LimitManager {
public:
  void begin(NVSManager &nvs,
             PowerCalculator &powerCalc, SystemData *sysData,
             SemaphoreHandle_t *mutex, Buzzer *buzzer);

  // Call from sensorTask (~80ms). Takes dataMutex internally.
  void loop();

  // Manual counter reset (user-triggered). Zeroes the channel's energy.
  void resetCounter(uint8_t ch);

  // Audit trail for silent energy writes (test_inject / inject). These
  // bypass resetCounter(), so without this they leave zero trace in the
  // event log. Takes dataMutex internally; never call while holding it.
  void logEnergyWrite(uint8_t ch, float v, const char *src);

private:
  NVSManager *nvs;
  PowerCalculator *powerCalc;
  SystemData *sysData;
  SemaphoreHandle_t *dataMutex;
  Buzzer *buzzer;

  bool tripNotified[NUM_CHANNELS];
  // Logged the one "Auto-recovered" event for the current trip. Prevents
  // re-logging the recovery every cycle while energy stays over budget.
  bool autoRecoverLogged[NUM_CHANNELS];
  // One-shot boot marker (see loop()): proves a restart happened and when,
  // so a counter drop can be attributed to reboot + NVS reload afterwards.
  bool bootEventLogged = false;
  uint8_t tripCycleIndex = 0;

  void logEvent(uint8_t ch, ChannelStatus s, const char *msg, float v);
  // Every event is RAM-logged AND flash-persisted (last FORENSIC_KEEP kept), so
  // a reboot or power cut can never erase the trail. Persisting commits
  // immediately - call ONLY with dataMutex already held (all callers do).
  void logForensicEvent(uint8_t ch, ChannelStatus s, const char *msg, float v);
  void persistForensic();
  void rolloverIfNeeded();
  void checkLimits();
  void updateBuzzer();
};

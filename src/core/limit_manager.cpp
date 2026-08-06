#include "limit_manager.h"

#include <time.h>

void LimitManager::begin(NVSManager &nvsRef,
                         PowerCalculator &powerCalcRef, SystemData *sysDataRef,
                         SemaphoreHandle_t *mutexRef, NtfyNotifier *ntfyRef,
                         Buzzer *buzzerRef) {
  nvs = &nvsRef;
  powerCalc = &powerCalcRef;
  sysData = sysDataRef;
  dataMutex = mutexRef;
  ntfy = ntfyRef;
  buzzer = buzzerRef;

  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    tripNotified[ch] = false;
  }
}

void LimitManager::loop() {
  if (!sysData || !dataMutex) return;
  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

  rolloverIfNeeded();
  checkLimits();
  updateBuzzer();

  xSemaphoreGive(*dataMutex);
}

void LimitManager::checkLimits() {
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    float energy = powerCalc->getEnergyKWh(ch);
    float limit = sysData->channels[ch].monthlyKwhLimit;

    if (limit > 0 && energy >= limit) {
      sysData->channels[ch].status = STATUS_TRIPPED;
      if (!tripNotified[ch]) {
        tripNotified[ch] = true;
        logEvent(ch, STATUS_TRIPPED, "Monthly limit reached — over budget", energy);
        if (ntfy) {
          char buf[96];
          snprintf(buf, sizeof(buf), "Ch%d reached its monthly limit (%.1f kWh)", ch + 1, energy);
          ntfy->notify("Electricity limit reached", buf);
        }
      }
    } else {
      sysData->channels[ch].status = STATUS_OK;
    }
  }
}

// While any channel is tripped, keep beeping forever (looping through every
// tripped channel's beep count) until all tripped channels are reset.
void LimitManager::updateBuzzer() {
  if (!buzzer) return;
  if (buzzer->isBusy()) return;

  uint8_t tripped[NUM_CHANNELS];
  uint8_t n = 0;
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    if (sysData->channels[ch].status == STATUS_TRIPPED) {
      tripped[n++] = ch;
    }
  }

  if (n == 0) return;

  if (tripCycleIndex >= n) tripCycleIndex = 0;
  buzzer->ring(tripped[tripCycleIndex] + 1);
  tripCycleIndex = (tripCycleIndex + 1) % n;
}

void LimitManager::rolloverIfNeeded() {
  time_t now = time(nullptr);
  if (now <= 1600000000) return;  // NTP not synced yet

  struct tm t;
  localtime_r(&now, &t);
  int32_t month = (t.tm_year + 1900) * 100 + (t.tm_mon + 1);

  if (month == nvs->loadLastMonth()) return;

  nvs->saveLastMonth(month);

  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    powerCalc->setEnergyKWh(ch, 0.0f);
    sysData->channels[ch].energyKWh = 0.0f;
    sysData->channels[ch].status = STATUS_OK;
    tripNotified[ch] = false;
  }
  if (buzzer) buzzer->stop();
  logEvent(0, STATUS_OK, "Monthly reset — counters zeroed", 0.0f);
}

void LimitManager::resetCounter(uint8_t ch) {
  if (ch >= NUM_CHANNELS) return;

  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

  powerCalc->resetEnergy(ch);
  sysData->channels[ch].energyKWh = 0.0f;
  sysData->channels[ch].status = STATUS_OK;
  tripNotified[ch] = false;
  if (buzzer) buzzer->stop();
  nvs->saveEnergyKWh(ch, 0.0f);
  logEvent(ch, STATUS_OK, "Manual reset — counter zeroed", 0.0f);

  xSemaphoreGive(*dataMutex);
}

void LimitManager::logEvent(uint8_t ch, ChannelStatus s, const char *msg, float v) {
  if (sysData->eventCount < EVENT_LOG_SIZE) {
    Event &ev = sysData->events[sysData->eventCount];
    ev.timestamp = (uint32_t)time(nullptr);
    ev.channel = ch;
    ev.status = s;
    ev.value = v;
    snprintf(ev.message, EVENT_MSG_LEN, "%s", msg);
    sysData->eventCount++;
  } else {
    for (int i = 1; i < EVENT_LOG_SIZE; i++) {
      sysData->events[i - 1] = sysData->events[i];
    }
    Event &ev = sysData->events[EVENT_LOG_SIZE - 1];
    ev.timestamp = (uint32_t)time(nullptr);
    ev.channel = ch;
    ev.status = s;
    ev.value = v;
    snprintf(ev.message, EVENT_MSG_LEN, "%s", msg);
  }
}

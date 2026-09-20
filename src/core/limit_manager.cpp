#include "limit_manager.h"

#include <time.h>
#include "../utils/log_gate.h"

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
    autoRecoverLogged[ch] = false;
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
      if (!tripNotified[ch]) {
        tripNotified[ch] = true;
        autoRecoverLogged[ch] = false;  // allow one recover log per trip
        logEvent(ch, STATUS_TRIPPED, "Monthly limit reached — over budget", energy);
        if (ntfy) {
          char buf[96];
          snprintf(buf, sizeof(buf), "Ch%d reached its monthly limit (%.1f kWh)", ch + 1, energy);
          ntfy->notify("Electricity limit reached", buf);
        }
      }
      if (powerCalc->getPowerFactor(ch) < AUTO_RECOVER_PF) {
        sysData->channels[ch].status = STATUS_OK;
        // Latch stays SET here: energy is still over budget, so clearing
        // it would re-arm the trip and re-log + re-notify + re-beep on the
        // very next cycle (event-log flood). Re-arm happens only in the
        // else branch, once energy is back under the limit.
        if (!autoRecoverLogged[ch]) {
          autoRecoverLogged[ch] = true;
          logEvent(ch, STATUS_OK, "Auto-recovered — load removed", energy);
        }
      } else {
        sysData->channels[ch].status = STATUS_TRIPPED;
      }
    } else {
      // Energy back under the limit (manual reset / monthly rollover):
      // re-arm the trip so the next over-budget excursion notifies again.
      tripNotified[ch] = false;
      autoRecoverLogged[ch] = false;
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
  static bool rolloverDoneThisBoot = false;
  if (rolloverDoneThisBoot) return;

  time_t now = time(nullptr);
  if (now <= 1600000000) return;  // NTP not synced yet

  struct tm t;
  localtime_r(&now, &t);
  int y = t.tm_year + 1900;
  int m = t.tm_mon + 1;
  int d = t.tm_mday;
  // Billing cycle anchored to MONTHLY_RESET_DAY (e.g. 26th → 25th).
  // Effective month increments on/after the reset day at 00:00 UTC.
  if (d >= MONTHLY_RESET_DAY) {
    m += 1;
    if (m > 12) { m = 1; y += 1; }
  }
  int32_t billingMonth = y * 100 + m;

  if (billingMonth == nvs->loadLastMonth()) return;

  rolloverDoneThisBoot = true;
  STATUS_LOG("  [ROLLOVER] billingMonth=%ld  (was %ld) — zeroing all counters\n",
                (long)billingMonth, (long)nvs->loadLastMonth());
  nvs->saveLastMonth(billingMonth);
  nvs->commit();  // persist immediately — prevents repeat on next boot

  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    powerCalc->setEnergyKWh(ch, 0.0f);
    sysData->channels[ch].energyKWh = 0.0f;
    sysData->channels[ch].status = STATUS_OK;
    tripNotified[ch] = false;
    autoRecoverLogged[ch] = false;
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
  autoRecoverLogged[ch] = false;
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

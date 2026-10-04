#include "limit_manager.h"

#include <time.h>
#include "../utils/log_gate.h"

void LimitManager::begin(NVSManager &nvsRef,
                         PowerCalculator &powerCalcRef, SystemData *sysDataRef,
                         SemaphoreHandle_t *mutexRef,
                         Buzzer *buzzerRef) {
  nvs = &nvsRef;
  powerCalc = &powerCalcRef;
  sysData = sysDataRef;
  dataMutex = mutexRef;
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

  // One-shot boot marker. Deferred until NTP time is valid so the event
  // carries a real timestamp (in setup() the clock is still ~1970).
  // Falls back after 5 min uptime so offline boards still record it.
  // Forensic-persisted like all critical events: proves a restart took
  // place even if the RAM log is wiped again afterwards.
  if (!bootEventLogged) {
    time_t now = time(nullptr);
    if (now > 1600000000 || millis() > 300000UL) {
      bootEventLogged = true;
      logForensicEvent(0, STATUS_OK, "Boot — NVS energies reloaded", 0.0f);
      STATUS_LOG("  [BOOT] event logged (epoch %ld)\n", (long)now);
    }
  }

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
        logForensicEvent(ch, STATUS_TRIPPED, "Monthly limit reached — over budget", energy);
        // No server-side push here any more: ntfy.sh needs the internet,
        // and this board has none. The trip is signalled by the buzzer, the
        // channel status field in the WebSocket snapshot, and this event -
        // which is what makes the dashboard raise a browser Notification.
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
  // No once-per-boot latch here on purpose. The persisted billing month IS the
  // idempotency guard (see the billingMonth == loadLastMonth() check below), so
  // re-evaluating the clock every cycle is safe and cheap. A "done this boot"
  // latch made an always-on board fire this exactly once — on the first valid
  // NTP, when the stored month is still 0 — and then never again, so the
  // monthly reset was silently skipped for the remainder of that boot session.
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

  STATUS_LOG("  [ROLLOVER] billingMonth=%ld  (was %ld) — zeroing all counters\n",
                (long)billingMonth, (long)nvs->loadLastMonth());
  nvs->saveLastMonth(billingMonth);

  // Persist the zeros in the SAME commit that advances the billing month.
  // Zeroing only in RAM used to let a reboot between here and the next periodic
  // save restore the old kWh from flash while last_month had already moved on —
  // losing the reset permanently.
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    powerCalc->setEnergyKWh(ch, 0.0f);
    sysData->channels[ch].energyKWh = 0.0f;
    sysData->channels[ch].status = STATUS_OK;
    nvs->saveEnergyKWh(ch, 0.0f);
    tripNotified[ch] = false;
    autoRecoverLogged[ch] = false;
  }
  nvs->commit();  // persist immediately — prevents repeat on next boot
  if (buzzer) buzzer->stop();
  logForensicEvent(0, STATUS_OK, "Monthly reset — counters zeroed", 0.0f);
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
  logForensicEvent(ch, STATUS_OK, "Manual reset — counter zeroed", 0.0f);

  xSemaphoreGive(*dataMutex);
}

void LimitManager::logEnergyWrite(uint8_t ch, float v, const char *src) {
  if (ch >= NUM_CHANNELS || !sysData || !dataMutex) return;

  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

  char msg[EVENT_MSG_LEN];
  snprintf(msg, sizeof(msg), "%s — energy set", src ? src : "Inject");
  logForensicEvent(ch, STATUS_OK, msg, v);

  xSemaphoreGive(*dataMutex);
}

void LimitManager::logForensicEvent(uint8_t ch, ChannelStatus s, const char *msg, float v) {
  // Kept as the name every call site uses; persistence now lives in logEvent
  // itself, so there is no longer a RAM-only vs persisted distinction to get
  // wrong at a new call site.
  logEvent(ch, s, msg, v);
}

void LimitManager::persistForensic() {
  if (!sysData || !nvs) return;
  // Snapshot the tail of the RAM ring (chronological) into flash.
  uint8_t total = sysData->eventCount;
  uint8_t keep = (total < NVSManager::FORENSIC_KEEP) ? total : NVSManager::FORENSIC_KEEP;
  if (keep == 0) return;
  // Ring buffer: index 0 is oldest, eventCount-1 newest (see logEvent).
  const Event *tail = &sysData->events[total - keep];
  nvs->saveForensicEvents(tail, keep);
  // Committed HERE, not at some later coincidental save: a trip followed by a
  // power cut must still leave its event in flash. Caller holds dataMutex (all
  // logEvent paths do), so this commit cannot race the sensorTask save.
  nvs->commit();
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
  // Every event lands in flash, not just trips: the RAM ring is wiped by any
  // reboot, and a power cut must not erase the last thing the board saw.
  persistForensic();
}

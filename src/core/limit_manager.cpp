#include "limit_manager.h"

#include <time.h>
#include "../utils/log_gate.h"
#include "../network/time_sync.h"

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
    autoRecovered[ch] = false;
    ringStable[ch] = 0;
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
        autoRecovered[ch] = false;      // a new trip starts un-recovered
        ringStable[ch] = 0;
        logForensicEvent(ch, STATUS_TRIPPED, "Monthly limit reached — over budget", energy);
        // No server-side push here any more: ntfy.sh needs the internet,
        // and this board has none. The trip is signalled by the buzzer, the
        // channel status field in the WebSocket snapshot, and this event -
        // which is what makes the dashboard raise a browser Notification.
      }
      // Silence is instant, ringing needs proof. A removed load pins PF at 0
      // via the deadband, so dropping below AUTO_RECOVER_PF (0.2) recovers
      // immediately. Re-ringing from a recovery needs PF above the line for
      // PF_RING_STABLE_CYCLES consecutive cycles (~1.5 s) — a flickering
      // noise spike resets the count every time it dips, so it can never
      // accumulate the 19 confirmations a real steady load delivers.
      float pf = powerCalc->getPowerFactor(ch);
      if (!autoRecovered[ch]) {
        if (pf < AUTO_RECOVER_PF) {
          autoRecovered[ch] = true;
          ringStable[ch] = 0;
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
        if (pf > AUTO_RECOVER_PF) {
          if (ringStable[ch] < 255) ringStable[ch]++;
        } else {
          ringStable[ch] = 0;
        }
        if (ringStable[ch] >= PF_RING_STABLE_CYCLES) {
          autoRecovered[ch] = false;
          autoRecoverLogged[ch] = false;  // next recovery logs its own episode
          ringStable[ch] = 0;
          sysData->channels[ch].status = STATUS_TRIPPED;
          logEvent(ch, STATUS_TRIPPED, "Load back — alarm resumed", energy);
        } else {
          sysData->channels[ch].status = STATUS_OK;
        }
      }
    } else {
      // Energy back under the limit (manual reset / monthly rollover):
      // re-arm the trip so the next over-budget excursion notifies again.
      tripNotified[ch] = false;
      autoRecoverLogged[ch] = false;
      autoRecovered[ch] = false;
      ringStable[ch] = 0;
      sysData->channels[ch].status = STATUS_OK;
    }
  }
}

// While any channel is tripped, keep beeping forever (looping through every
// tripped channel's beep count) until all tripped channels are reset. The
// driver's 1s end-pause separates each round, so neighbouring channels never
// blur into one long count.
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

// Billing-cycle math shared by rolloverIfNeeded() and setResetDay(): at 00:00
// local time (UTC+7) on the reset day the counters zero and the new billing month begins
// (reset day 25: the 25th itself starts the new cycle, 25th → 24th).
static int32_t billingMonthFor(int y, int m, int d, int resetDay) {
  if (d >= resetDay) {
    m += 1;
    if (m > 12) { m = 1; y += 1; }
  }
  return y * 100 + m;
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

  // Live-clock sanity: refuse to make a month decision while the running
  // clock disagrees with the last accepted track by more than 5 minutes.
  // pollNTP() re-adopts a sustained correction (so this clears on its own),
  // but while the clock is suspect we pause instead of zeroing real
  // accumulation on a bogus boundary.
  {
    int64_t drift = timeSync.liveTrackDriftSeconds();
    if (drift > 300 || drift < -300) {
      static int64_t lastGuardLogAt = 0;
      int64_t nowSec = (int64_t)time(nullptr);
      if (nowSec - lastGuardLogAt > 3600) {
        lastGuardLogAt = nowSec;
        STATUS_LOG("  [TIME] clock/track drift %+llds — rollover paused\n",
                   (long long)drift);
      }
      return;
    }
  }

  struct tm t;
  localtime_r(&now, &t);
  int32_t billingMonth = billingMonthFor(t.tm_year + 1900, t.tm_mon + 1,
                                         t.tm_mday, resetDay());

  int32_t marker = nvs->loadLastMonth();
  if (billingMonth == marker) return;

  // An UNSET marker (0) is not a month to roll over from — it is a fresh
  // board, a never-written namespace, or an unreadable read (NVS handle
  // closed mid-commit on the other core: closed reads return the default 0,
  // see Preferences::getInt). Wiping here would destroy real accumulation on
  // zero evidence — the 2026-10-05 reboot wipes read exactly this 0. With no
  // previous month to compare against there is nothing to close out, so
  // anchor the marker to the current cycle and keep every counter. A true
  // fresh board holds 0.0 kWh in RAM anyway, so anchoring is observably
  // identical to wiping there; the next real month boundary still fires.
  if (marker == 0) {
    STATUS_LOG("  [ROLLOVER] marker unset — anchoring to %ld, counters kept\n",
               (long)billingMonth);
    nvs->saveLastMonth(billingMonth);
    nvs->commit();
    logForensicEvent(0, STATUS_OK, "Billing anchor initialized — counters kept", 0.0f);
    return;
  }

  // Forward only. A marker AHEAD of the computed month means the clock moved
  // backward since the marker was written (NTP/browser correction landing) or
  // the marker came from the future — wiping here would destroy real
  // accumulation because of a correction, so re-anchor silently instead and
  // say so in the log. test_force_rollover still works: it sets the marker
  // BACK, which reads as forward progress and fires below.
  if (billingMonth < marker) {
    STATUS_LOG("  [ROLLOVER] billingMonth=%ld behind marker %ld — re-anchoring, counters kept\n",
               (long)billingMonth, (long)marker);
    nvs->saveLastMonth(billingMonth);
    nvs->commit();
    logForensicEvent(0, STATUS_OK, "Billing marker moved backward — re-anchored, counters kept", 0.0f);
    return;
  }

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
    autoRecovered[ch] = false;
    ringStable[ch] = 0;
  }
  nvs->commit();  // persist immediately — prevents repeat on next boot
  if (buzzer) buzzer->stop();
  logForensicEvent(0, STATUS_OK, "Monthly reset — counters zeroed", 0.0f);
}

uint8_t LimitManager::resetDay() {
  if (!nvs) return MONTHLY_RESET_DAY;
  return nvs->loadResetDay();
}

bool LimitManager::setResetDay(uint8_t day) {
  if (day < 1 || day > 28 || !nvs || !dataMutex) return false;
  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  nvs->saveResetDay(day);
  // Re-anchor the cycle to the new day WITHOUT zeroing: the stored marker is
  // computed under the old day, so without this the next check would read the
  // change as a new billing month and wipe the counters as a side effect.
  // When the clock is not set yet there is nothing sane to anchor to — the
  // first valid time then behaves like a fresh board (one rollover, already
  // zero), which is the pre-existing behaviour, not a new edge.
  time_t now = time(nullptr);
  if (now > 1600000000) {
    // Same guard as rolloverIfNeeded: never re-anchor the cycle onto a
    // clock we do not trust. The new day still saves; the next reliable
    // month boundary re-anchors from it.
    int64_t drift = timeSync.liveTrackDriftSeconds();
    if (drift <= 300 && drift >= -300) {
      struct tm t;
      localtime_r(&now, &t);
      nvs->saveLastMonth(billingMonthFor(t.tm_year + 1900, t.tm_mon + 1,
                                         t.tm_mday, day));
    }
  }
  nvs->commit();
  char msg[EVENT_MSG_LEN];
  snprintf(msg, sizeof(msg), "Billing reset day set to %d", (int)day);
  // Logged while STILL holding dataMutex: logForensicEvent persists to flash
  // with its own commit, which is not thread-safe against a concurrent one.
  logForensicEvent(0, STATUS_OK, msg, (float)day);
  xSemaphoreGive(*dataMutex);
  return true;
}

void LimitManager::resetCounter(uint8_t ch) {
  if (ch >= NUM_CHANNELS) return;

  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

  powerCalc->resetEnergy(ch);
  sysData->channels[ch].energyKWh = 0.0f;
  sysData->channels[ch].status = STATUS_OK;
  tripNotified[ch] = false;
  autoRecoverLogged[ch] = false;
  autoRecovered[ch] = false;
  ringStable[ch] = 0;
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

void LimitManager::logOTAVerified() {
  if (!sysData || !dataMutex) return;
  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  logForensicEvent(0, STATUS_OK, "OTA verified — network up", 0.0f);
  xSemaphoreGive(*dataMutex);
}

void LimitManager::logEcoSleep() {
  if (!sysData || !dataMutex) return;
  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  logForensicEvent(0, STATUS_OK, "Eco on — radio idling (no viewers)", 0.0f);
  xSemaphoreGive(*dataMutex);
}

void LimitManager::logEcoWake() {
  if (!sysData || !dataMutex) return;
  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  logForensicEvent(0, STATUS_OK, "Eco off — viewer back, full power", 0.0f);
  xSemaphoreGive(*dataMutex);
}

void LimitManager::auditForceRollover() {
  if (!sysData || !dataMutex) return;

  if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

  logForensicEvent(0, STATUS_OK, "Rollover test armed — next cycle zeroes counters", 0.0f);

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

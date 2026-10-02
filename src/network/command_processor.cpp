#include "command_processor.h"
#include "../core/limit_manager.h"
#include "console_handler.h"
#include "time_sync.h"
#include "../utils/log_gate.h"

// Reverses the JSON string escaping applied by the dashboard when it sends a
// console line (JSON.stringify escapes ", \\ and control chars).
static String jsonUnescape(const String &s) {
  String r;
  r.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' && i + 1 < s.length()) {
      char n = s[++i];
      if (n == 'n') r += '\n';
      else if (n == 't') r += '\t';
      else if (n == 'r') r += '\r';
      else r += n;  // covers \" \\ \/
    } else {
      r += c;
    }
  }
  return r;
}

// Extracts the value of a JSON string field, honouring backslash escapes so an
// embedded \" does not terminate the scan early.
static bool extractJsonString(const String &s, const char *key, String &outVal) {
  String needle = String("\"") + key + "\":\"";
  int i = s.indexOf(needle);
  if (i < 0) return false;
  i += needle.length();
  int end = i;
  while (end < (int)s.length()) {
    if (s[end] == '\\') { end += 2; continue; }
    if (s[end] == '"') break;
    end++;
  }
  if (end > (int)s.length()) return false;
  outVal = jsonUnescape(s.substring(i, end));
  return true;
}

// Verbs that are deliberately NOT PIN-gated.
//   set_time    - every viewer sends this on connect; gating it would mean the
//                 monthly rollover stays dormant until someone unlocks the UI.
//   verify_pin  - its whole job is to answer the PIN question.
// Anything not listed here is treated as mutating and requires a valid PIN.
static const char *const PIN_EXEMPT_VERBS[] = {"set_time", "verify_pin", nullptr};

// True if `verb` mutates device state and therefore needs the admin PIN.
static bool requiresPin(const char *verb) {
  for (const char *const *v = PIN_EXEMPT_VERBS; *v; v++) {
    if (strcmp(*v, verb) == 0) return false;
  }
  return true;
}

// Pulls the verb out of a frame without allocating a String per candidate.
static String commandVerb(const String &s) {
  String needle = "\"cmd\":\"";
  int i = s.indexOf(needle);
  if (i < 0) return String();
  i += needle.length();
  int end = i;
  while (end < (int)s.length() && s[end] != '"') end++;
  if (end >= (int)s.length()) return String();
  return s.substring(i, end);
}

// Shared command handler for the WebSocket server.
// Mirrors the command vocabulary of the frontend dashboard:
// set_name, reset_counter, test_inject, set_voltage_cal, set_current_cal,
// set_monthly_kwh, set_noise_floor, set_lpf, set_rms_samples, set_ntfy_topic,
// set_ntfy_enabled, reset_ch_cal, reset_ch_to_default, reset_nvs_defaults,
// test_force_rollover, set_time.
//
// (The ntfy verbs still parse and are still accepted so an old dashboard
// build does not error, but ntfy.sh is unreachable in AP-only mode so they
// only persist a setting nothing reads. Phase 6 removes them.)
bool processCommand(NVSManager *nvs, SystemData *sysData,
                    SemaphoreHandle_t *dataMutex,
                    PowerCalculator *powerCalc, LimitManager *limitMgr,
                    const char *msg, String *responseOut, bool *authRejected) {
  bool handled = false;
  String s(msg);

  if (authRejected) *authRejected = false;

  // --- Admin gate, evaluated before any verb touches state --------------
  // Placed here rather than inside each branch so a new mutating verb is
  // protected by default instead of by remembering to add a PIN check.
  {
    String verb = commandVerb(s);
    if (verb.length() > 0 && requiresPin(verb.c_str())) {
      String want = nvs->loadPin();
      String got;
      extractJsonString(s, "pin", got);
      if (got.length() == 0 || got != want) {
        if (authRejected) *authRejected = true;
        if (responseOut) {
          *responseOut = String("  Admin PIN required (Settings -> Admin PIN)");
        }
        DEBUG_LOG("  [AUTH] rejected '%s' — PIN missing or wrong\n", verb.c_str());
        return false;
      }
    }
  }

  // DEBUG: log voltageCal before command processing
  if (powerCalc) {
    DEBUG_LOG("  [CMD] voltageCal before cmd: %.1f  msg: %.40s\n",
                  powerCalc->voltageCal, msg);
  }

  if (s.indexOf("\"cmd\":\"console\"") >= 0) {
    String line;
    if (extractJsonString(s, "line", line)) {
      if (responseOut) {
        consoleHandler.exec(line, *responseOut);
      } else {
        String discard;
        consoleHandler.exec(line, discard);
      }
      handled = true;
    }

  } else if (s.indexOf("\"cmd\":\"set_name\"") >= 0) {
    int ch = -1;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int ni = s.indexOf("\"name\":\"");
    if (ni >= 0 && ch >= 0 && ch < NUM_CHANNELS) {
      ni += 8;
      int end = s.indexOf("\"", ni);
      if (end > ni) {
        String name = s.substring(ni, end);
        if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          nvs->saveChannelName(ch, name.c_str());
          strncpy(sysData->channels[ch].name, name.c_str(), MAX_CHANNEL_NAME_LEN - 1);
          sysData->channels[ch].name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
          xSemaphoreGive(*dataMutex);
        }
        handled = true;
      }
    }

  } else if (s.indexOf("\"cmd\":\"reset_counter\"") >= 0) {
    int ch = -1;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    if (ch >= 0 && ch < NUM_CHANNELS && limitMgr) {
      limitMgr->resetCounter((uint8_t)ch);
      handled = true;
    }

  } else if (s.indexOf("\"cmd\":\"test_inject\"") >= 0) {
    int ch = -1; float val = 0;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) val = s.substring(vi + 6).toFloat();
    if (ch >= 0 && ch < NUM_CHANNELS && val >= 0 && powerCalc) {
      powerCalc->setEnergyKWh(ch, val);
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->channels[ch].energyKWh = val;
        xSemaphoreGive(*dataMutex);
      }
      // Audited: previously this silent write left zero trace in the log.
      if (limitMgr) limitMgr->logEnergyWrite((uint8_t)ch, val, "Test inject");
      handled = true;
    }

  } else if (s.indexOf("\"cmd\":\"set_voltage_cal\"") >= 0) {
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) {
      float val = s.substring(vi + 6).toFloat();
      nvs->saveVoltageCalibration(val);
      if (powerCalc) powerCalc->voltageCal = val;
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->voltageCalibration = val;
        xSemaphoreGive(*dataMutex);
      }
      handled = true;
    }

  } else if (s.indexOf("\"cmd\":\"set_current_cal\"") >= 0) {
    int ch = -1; float val = 0;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) val = s.substring(vi + 6).toFloat();
    if (ch >= 0 && ch < NUM_CHANNELS && val > 0 && powerCalc) {
      powerCalc->currentCal[ch] = val;
      nvs->saveChannelCurrentCal(ch, val);
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->currentCalibration[ch] = val;
        xSemaphoreGive(*dataMutex);
      }
      handled = true;
    }

  } else if (s.indexOf("\"cmd\":\"set_monthly_kwh\"") >= 0) {
    int ch = -1; float val = 0;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) val = s.substring(vi + 6).toFloat();
    if (ch >= 0 && ch < NUM_CHANNELS && val > 0) {
      nvs->saveMonthlyKwhLimit(ch, val);
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->channels[ch].monthlyKwhLimit = val;
        xSemaphoreGive(*dataMutex);
      }
      handled = true;
    }

  } else if (s.indexOf("\"cmd\":\"set_noise_floor\"") >= 0) {
    int ch = -1;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    if (ch >= 0 && ch < NUM_CHANNELS) {
      int vi = s.indexOf("\"val\":");
      if (vi >= 0) {
        float val = s.substring(vi + 6).toFloat();
        powerCalc->setNoiseFloor(ch, val);
        nvs->saveNoiseFloor(ch, val);
      } else {
        powerCalc->requestAutoZero(ch);
      }
      handled = true;
    }

  } else if (s.indexOf("\"cmd\":\"set_lpf\"") >= 0) {
    int ch = -1; float val = 0;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) val = s.substring(vi + 6).toFloat();
    if (ch >= 0 && ch < NUM_CHANNELS && val > 0 && powerCalc) {
      powerCalc->setLpfAlpha(ch, val);  // clamps to 0.01 .. 1.0 internally
      nvs->saveLpfAlpha(ch, powerCalc->lpfAlpha[ch]);
      handled = true;
    }

  } else if (s.indexOf("\"cmd\":\"set_rms_samples\"") >= 0) {
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) {
      uint16_t val = (uint16_t)s.substring(vi + 6).toInt();
      if (val >= 100 && val <= MAX_RMS_SAMPLES && powerCalc) {
        powerCalc->setRmsSamples(val);
        nvs->saveRmsSamples(val);
        if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sysData->rmsSamples = val;
          xSemaphoreGive(*dataMutex);
        }
        handled = true;
      }
    }

  } else if (s.indexOf("\"cmd\":\"set_ntfy_topic\"") >= 0) {
    int vi = s.indexOf("\"val\":\"");
    if (vi >= 0) {
      vi += 7;
      int end = s.indexOf("\"", vi);
      if (end > vi) {
        String topic = s.substring(vi, end);
        nvs->saveNtfyTopic(topic);
        handled = true;
      }
    }

  } else if (s.indexOf("\"cmd\":\"set_ntfy_enabled\"") >= 0) {
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) {
      bool on = s.substring(vi + 6, vi + 10) == "true";
      nvs->saveNtfyEnabled(on);
      handled = true;
    }

  } else if (s.indexOf("\"cmd\":\"reset_ch_cal\"") >= 0) {
    int ch = -1;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    if (ch >= 0 && ch < NUM_CHANNELS) {
      DEBUG_LOG("  [RESET_CH_CAL] ch=%d  voltageCal BEFORE=%.1f\n", ch, powerCalc->voltageCal);
      nvs->saveChannelCurrentCal(ch, DEFAULT_CURRENT_CALIBRATION);
      nvs->saveNoiseFloor(ch, 0.0f);
      nvs->saveLpfAlpha(ch, DEFAULT_LPF_ALPHA);
      powerCalc->currentCal[ch] = DEFAULT_CURRENT_CALIBRATION;
      powerCalc->setNoiseFloor(ch, 0.0f);
      powerCalc->setLpfAlpha(ch, DEFAULT_LPF_ALPHA);
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->currentCalibration[ch] = DEFAULT_CURRENT_CALIBRATION;
        xSemaphoreGive(*dataMutex);
      }
      DEBUG_LOG("  [RESET_CH_CAL] ch=%d  voltageCal AFTER=%.1f\n", ch, powerCalc->voltageCal);
      handled = true;
    }

  } else if (s.indexOf("\"cmd\":\"reset_channel_names\"") >= 0 || s.indexOf("\"cmd\":\"reset_ch_to_default\"") >= 0) {
    // A channel is ALWAYS required. The dashboard always sends "ch"; a
    // command without a valid channel is ignored instead of being treated as
    // "reset every channel" (previously a flattened child object dropped the
    // "ch" field and wiped all 6 channels' limits).
    int ch = -1;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    if (ch < 0 || ch >= NUM_CHANNELS) {
      DEBUG_LOG("  [CMD] %s ignored: ch=%d out of range\n",
                    s.indexOf("\"cmd\":\"reset_channel_names\"") >= 0 ? "reset_channel_names" : "reset_ch_to_default", ch);
      handled = true;
    } else {
      nvs->clearChannelName(ch);
      nvs->saveMonthlyKwhLimit(ch, DEFAULT_MONTHLY_KWH_LIMIT);
    }
    if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (ch >= 0 && ch < NUM_CHANNELS) {
        strncpy(sysData->channels[ch].name, NVSManager::defaultChannelName(ch), MAX_CHANNEL_NAME_LEN - 1);
        sysData->channels[ch].name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
        sysData->channels[ch].monthlyKwhLimit = DEFAULT_MONTHLY_KWH_LIMIT;
      }
      xSemaphoreGive(*dataMutex);
    }
    handled = true;

  } else if (s.indexOf("\"cmd\":\"reset_nvs_defaults\"") >= 0) {
    nvs->saveVoltageCalibration(DEFAULT_VOLTAGE_CALIBRATION);
    nvs->saveRmsSamples(MAX_RMS_SAMPLES / 2);
    powerCalc->voltageCal = DEFAULT_VOLTAGE_CALIBRATION;
    powerCalc->rmsSamples = MAX_RMS_SAMPLES / 2;
    powerCalc->setRmsSamples(MAX_RMS_SAMPLES / 2);
    for (int i = 0; i < NUM_CHANNELS; i++) {
      nvs->saveChannelCurrentCal(i, DEFAULT_CURRENT_CALIBRATION);
      nvs->saveNoiseFloor(i, 0.0f);
      nvs->saveLpfAlpha(i, DEFAULT_LPF_ALPHA);
      powerCalc->currentCal[i] = DEFAULT_CURRENT_CALIBRATION;
      powerCalc->setNoiseFloor(i, 0.0f);
      powerCalc->setLpfAlpha(i, DEFAULT_LPF_ALPHA);
    }
    if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      sysData->voltageCalibration = DEFAULT_VOLTAGE_CALIBRATION;
      sysData->rmsSamples = powerCalc->rmsSamples;
      for (int i = 0; i < NUM_CHANNELS; i++) {
        sysData->currentCalibration[i] = DEFAULT_CURRENT_CALIBRATION;
      }
      xSemaphoreGive(*dataMutex);
    }
    handled = true;
  } else if (s.indexOf("\"cmd\":\"verify_pin\"") >= 0) {
    // Explicit "is my PIN right?" round trip so the dashboard can enable or
    // disable its admin controls without guessing from a failed command.
    String pin;
    extractJsonString(s, "pin", pin);
    bool ok = (pin.length() > 0 && pin == nvs->loadPin());
    if (responseOut) *responseOut = ok ? "  PIN OK" : "  PIN incorrect";
    handled = true;

  } else if (s.indexOf("\"cmd\":\"set_pin\"") >= 0) {
    // Gated like everything else mutating: changing the PIN requires knowing
    // the current one, checked above.
    String pin;
    if (extractJsonString(s, "pin_new", pin)) {
      pin.trim();
      if (pin.length() < 4 || pin.length() > 16) {
        if (responseOut) *responseOut = "  PIN must be 4-16 characters";
      } else {
        nvs->savePin(pin);
        if (responseOut) *responseOut = "  Admin PIN changed";
        handled = true;
      }
    }

  } else if (s.indexOf("\"cmd\":\"set_time\"") >= 0) {
    // The browser lends us its clock. Sent automatically on WebSocket open, so
    // it is deliberately NOT PIN-gated: a read-only viewer connecting is
    // exactly the event that should give the board a valid time, otherwise
    // the monthly rollover stays dormant until somebody types a PIN.
    int ti = s.indexOf("\"t\":");
    if (ti >= 0) {
      // toInt() stops at the first non-digit, so the trailing '}' is ignored.
      int64_t epoch = s.substring(ti + 4).toInt();
      if (timeSync.setTimeFromBrowser(epoch)) {
        handled = true;
        if (responseOut) {
          char buf[64];
          snprintf(buf, sizeof(buf), "  Clock set to epoch %lld", (long long)epoch);
          *responseOut = String(buf);
        }
      }
    }

  } else if (s.indexOf("\"cmd\":\"test_force_rollover\"") >= 0) {
    nvs->saveLastMonth(202607);
    handled = true;
  }

  if (handled) nvs->commit();

  // DEBUG: log voltageCal after every handled command to detect corruption
  if (handled && powerCalc) {
    DEBUG_LOG("  [CMD] voltageCal after cmd: %.1f\n", powerCalc->voltageCal);
  }

  return handled;
}
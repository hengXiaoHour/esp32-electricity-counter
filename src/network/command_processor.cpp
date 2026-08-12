#include "command_processor.h"
#include "../core/limit_manager.h"
#include "console_handler.h"

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

// Shared command handler for both the WebSocket server and the Firebase bridge.
// Mirrors the command vocabulary of the frontend dashboard:
// set_name, reset_counter, test_inject, set_voltage_cal, set_current_cal,
// set_monthly_kwh, set_noise_floor, set_lpf, set_rms_samples, set_ntfy_topic,
// set_ntfy_enabled, reset_ch_to_default, reset_nvs_defaults, test_force_rollover.
bool processCommand(NVSManager *nvs, SystemData *sysData,
                    SemaphoreHandle_t *dataMutex,
                    PowerCalculator *powerCalc, LimitManager *limitMgr,
                    const char *msg, String *responseOut) {
  bool handled = false;
  String s(msg);

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

  } else if (s.indexOf("\"cmd\":\"reset_channel_names\"") >= 0 || s.indexOf("\"cmd\":\"reset_ch_to_default\"") >= 0) {
    int ch = -1;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int startCh = (ch >= 0 && ch < NUM_CHANNELS) ? ch : 0;
    int endCh = (ch >= 0 && ch < NUM_CHANNELS) ? ch + 1 : NUM_CHANNELS;
    for (int i = startCh; i < endCh; i++) {
      nvs->clearChannelName(i);
      nvs->saveMonthlyKwhLimit(i, DEFAULT_MONTHLY_KWH_LIMIT);
    }
    if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      for (int i = startCh; i < endCh; i++) {
        strncpy(sysData->channels[i].name, NVSManager::defaultChannelName(i), MAX_CHANNEL_NAME_LEN - 1);
        sysData->channels[i].name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
        sysData->channels[i].monthlyKwhLimit = DEFAULT_MONTHLY_KWH_LIMIT;
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
      nvs->saveLpfAlpha(i, 1.0f);
      powerCalc->currentCal[i] = DEFAULT_CURRENT_CALIBRATION;
      powerCalc->setNoiseFloor(i, 0.0f);
      powerCalc->setLpfAlpha(i, 1.0f);
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
  } else if (s.indexOf("\"cmd\":\"test_force_rollover\"") >= 0) {
    nvs->saveLastMonth(202607);
    handled = true;
  }

  if (handled) nvs->commit();
  return handled;
}
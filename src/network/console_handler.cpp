#include "console_handler.h"

#include <stdarg.h>

#include "../core/limit_manager.h"
#include "../core/power_calculator.h"
#include "../network/ota_handler.h"
#include "../network/wifi_manager.h"
#include "../ui/buzzer.h"
#include "../utils/nvs_manager.h"

ConsoleHandler consoleHandler;

String consoleJsonEscape(const String &s) {
  String r;
  r.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') r += "\\\"";
    else if (c == '\\') r += "\\\\";
    else if (c == '\n') r += "\\n";
    else if (c == '\r') r += "\\r";
    else if (c == '\t') r += "\\t";
    else if ((uint8_t)c < 0x20) continue;  // drop other control chars
    else r += c;
  }
  return r;
}

void consoleAppendf(String &out, const char *fmt, ...) {
  if (out.length() >= ConsoleHandler::MAX_OUTPUT) return;
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  out += buf;
  out += '\n';
  if (out.length() > ConsoleHandler::MAX_OUTPUT) {
    out.remove(ConsoleHandler::MAX_OUTPUT);
    out += "\n... output truncated\n";
  }
}

static const char *statusName(ChannelStatus s) {
  switch (s) {
    case STATUS_OK:      return "OK";
    case STATUS_WARNING: return "WARN";
    case STATUS_TRIPPED: return "TRIP";
    default:             return "OFF";
  }
}

ConsoleHandler::ConsoleHandler()
  : nvs(nullptr), powerCalc(nullptr), sysData(nullptr), dataMutex(nullptr),
    buzzer(nullptr), limitMgr(nullptr), wifiMgr(nullptr), otaHandler(nullptr),
    ready(false) {}

void ConsoleHandler::begin(NVSManager *nvsRef, PowerCalculator *powerCalcRef,
                           SystemData *sysDataRef, SemaphoreHandle_t *mutexRef,
                           Buzzer *buzzerRef, LimitManager *limitMgrRef,
                           WiFiManager *wifiMgrRef, OTAHandler *otaHandlerRef) {
  nvs = nvsRef;
  powerCalc = powerCalcRef;
  sysData = sysDataRef;
  dataMutex = mutexRef;
  buzzer = buzzerRef;
  limitMgr = limitMgrRef;
  wifiMgr = wifiMgrRef;
  otaHandler = otaHandlerRef;
  ready = (nvs && powerCalc && sysData && dataMutex);
}

void ConsoleHandler::cmdStatus(String &out) {
  uint32_t up = millis() / 1000;
  consoleAppendf(out, "  %-16s%02lu:%02lu:%02lu", "Uptime:",
                 (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60),
                 (unsigned long)(up % 60));
  const char *wifiState = "DISCONNECTED";
  int rssi = 0;
  if (wifiMgr) {
    wifiState = wifiMgr->isConnected() ? "CONNECTED"
              : wifiMgr->isApMode()    ? "AP MODE" : "DISCONNECTED";
    rssi = wifiMgr->getRSSI();
  }
  consoleAppendf(out, "  %-16s%s  (RSSI: %d dBm)", "WiFi:", wifiState, rssi);
  consoleAppendf(out, "  %-16s%.1f V", "Voltage:", powerCalc->getVoltageRMS());
  consoleAppendf(out, "  %-16s%s (%d%%)", "OTA:",
                 (otaHandler && otaHandler->isInProgress()) ? "IN PROGRESS" : "IDLE",
                 otaHandler ? otaHandler->getProgress() : 0);
  consoleAppendf(out, "%s", "");
  consoleAppendf(out, "%s", "  Channels");
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    const ChannelData &c = sysData->channels[ch];
    consoleAppendf(out,
      "  Ch%d  %-16s %s  %5.2fA  %5.0fW  %5.0fVA  PF=%.3f  %6.3fkWh",
      ch + 1, c.name, statusName(c.status), c.currentRMS, c.activePower,
      c.apparentPower, c.powerFactor, c.energyKWh);
  }
  consoleAppendf(out, "  Events: %d", sysData->eventCount);
}

void ConsoleHandler::cmdChannel(int ch, String &out) {
  if (ch < 0 || ch >= NUM_CHANNELS) {
    consoleAppendf(out, "  Usage: ch <N> (1-%d)", NUM_CHANNELS);
    return;
  }
  const ChannelData &c = sysData->channels[ch];
  consoleAppendf(out, "  Channel %d  %s", ch + 1, c.name);
  consoleAppendf(out, "  %-16s%.2f A", "Current:", powerCalc->getCurrentRMS(ch));
  consoleAppendf(out, "  %-16s%.1f W", "Active Power:", powerCalc->getActivePower(ch));
  consoleAppendf(out, "  %-16s%.1f VA", "Apparent:", powerCalc->getApparentPower(ch));
  consoleAppendf(out, "  %-16s%.3f", "Power Factor:", powerCalc->getPowerFactor(ch));
  consoleAppendf(out, "  %-16s%.3f kWh", "Energy:", powerCalc->getEnergyKWh(ch));
  consoleAppendf(out, "  %-16s%s", "Status:", statusName(c.status));
  consoleAppendf(out, "  %-16s%.1f", "Current Cal:", powerCalc->currentCal[ch]);
  consoleAppendf(out, "  %-16s%.3f A", "Noise Floor:", powerCalc->noiseFloor[ch]);
}

void ConsoleHandler::cmdCal(String &out) {
  consoleAppendf(out, "  %-20s%d", "RMS Samples:", powerCalc->rmsSamples);
  consoleAppendf(out, "  %-20s%.1f", "Voltage Cal:", powerCalc->voltageCal);
  consoleAppendf(out, "%s", "  Current Calibration (A):");
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    consoleAppendf(out, "    Ch%d: %.1f  | Noise Floor: %.3f A",
                   ch + 1, powerCalc->currentCal[ch], powerCalc->noiseFloor[ch]);
  }
}

void ConsoleHandler::cmdInfo(String &out) {
  consoleAppendf(out, "  %-16sESP32-S3 Electricity Counter v%s", "Firmware:", FIRMWARE_VERSION);
  consoleAppendf(out, "  %-16s%s %s", "Built:", __DATE__, __TIME__);
  consoleAppendf(out, "  %-16s%d MB Flash, PSRAM %s", "Hardware:",
                 (int)(ESP.getFlashChipSize() / (1024 * 1024)), psramFound() ? "OK" : "N/A");
  consoleAppendf(out, "  %-16s%d MHz dual-core", "CPU:", (int)getCpuFrequencyMhz());
  consoleAppendf(out, "  %-16s%d", "Channels:", NUM_CHANNELS);
  consoleAppendf(out, "  %-16s%d-bit, %.1fV ref", "ADC:", ADC_RESOLUTION, ADC_REFERENCE_V);
  consoleAppendf(out, "  %-16s%u bytes", "Free heap:", (unsigned)ESP.getFreeHeap());
}

void ConsoleHandler::cmdWifi(String &out) {
  String ssid, pass;
  nvs->loadWiFi(ssid, pass);
  uint8_t mode = nvs->loadWiFiMode();
  const char *modeStr[] = {"AUTO", "STA", "AP"};
  consoleAppendf(out, "  %-16s%s", "Mode:", mode <= 2 ? modeStr[mode] : "?");
  consoleAppendf(out, "  %-16s\"%s\"", "SSID:", ssid.c_str());
  const char *state = "DISCONNECTED";
  int rssi = 0;
  if (wifiMgr) {
    state = wifiMgr->isConnected() ? "CONNECTED"
          : wifiMgr->isApMode()    ? "AP MODE" : "DISCONNECTED";
    rssi = wifiMgr->getRSSI();
  }
  consoleAppendf(out, "  %-16s%s", "Status:", state);
  consoleAppendf(out, "  %-16s%d dBm", "RSSI:", rssi);
  consoleAppendf(out, "  %-16s%s", "IP:", WiFi.localIP().toString().c_str());
  if (wifiMgr && wifiMgr->isApMode()) {
    consoleAppendf(out, "  %-16s%s", "AP IP:", WiFi.softAPIP().toString().c_str());
  }
}

void ConsoleHandler::cmdHelp(String &out) {
  consoleAppendf(out, "%s", "  Commands:");
  consoleAppendf(out, "%s", "    help                Show available commands");
  consoleAppendf(out, "%s", "    status              System status overview");
  consoleAppendf(out, "%s", "    ch <N>              Channel details (1-6)");
  consoleAppendf(out, "%s", "    cal                 Show calibration values");
  consoleAppendf(out, "%s", "    info                Firmware & hardware info");
  consoleAppendf(out, "%s", "    wifi                Show WiFi status");
  consoleAppendf(out, "%s", "    buzz <N>            Ring buzzer N beeps (1-6)");
  consoleAppendf(out, "%s", "    inject <ch> <kwh>   Set channel energy (testing)");
  consoleAppendf(out, "%s", "    reset <N>           Reset counter for channel (1-6)");
  consoleAppendf(out, "%s", "    reset_name [N]      Reset channel name(s) to default");
  consoleAppendf(out, "%s", "  Serial-only: test led, nvs_debug, reboot, setwifi, clearwifi");
}

void ConsoleHandler::exec(const String &line, String &out) {
  if (!ready) {
    consoleAppendf(out, "%s", "  Console not initialized");
    return;
  }

  String cmd(line);
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd == "status") {
    cmdStatus(out);

  } else if (cmd.startsWith("ch ")) {
    cmdChannel(cmd.substring(3).toInt() - 1, out);

  } else if (cmd == "cal") {
    cmdCal(out);

  } else if (cmd == "info") {
    cmdInfo(out);

  } else if (cmd == "wifi") {
    cmdWifi(out);

  } else if (cmd == "help" || cmd == "?") {
    cmdHelp(out);

  } else if (cmd.startsWith("buzz ")) {
    int n = cmd.substring(5).toInt();
    if (n >= 1 && n <= 6 && buzzer) {
      buzzer->ring((uint8_t)n);
      consoleAppendf(out, "  Buzzer ringing %d beeps", n);
    } else {
      consoleAppendf(out, "%s", "  Usage: buzz <N> (1-6 beeps)");
    }

  } else if (cmd.startsWith("inject ")) {
    int sp1 = cmd.indexOf(' ', 7);
    if (sp1 > 0) {
      int ch = cmd.substring(7, sp1).toInt() - 1;
      float kwh = cmd.substring(sp1 + 1).toFloat();
      if (ch >= 0 && ch < NUM_CHANNELS && kwh >= 0) {
        powerCalc->setEnergyKWh(ch, kwh);
        if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sysData->channels[ch].energyKWh = kwh;
          xSemaphoreGive(*dataMutex);
        }
        consoleAppendf(out, "  Ch%d energy injected: %.3f kWh", ch + 1, kwh);
      } else {
        consoleAppendf(out, "%s", "  Usage: inject <ch 1-6> <kwh>");
      }
    } else {
      consoleAppendf(out, "%s", "  Usage: inject <ch 1-6> <kwh>");
    }

  } else if (cmd.startsWith("reset_name")) {
    int ch = -1;
    if (cmd.length() > 11) ch = cmd.substring(11).toInt() - 1;
    if (ch >= 0 && ch < NUM_CHANNELS) {
      nvs->clearChannelName(ch);
      if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(sysData->channels[ch].name, NVSManager::defaultChannelName(ch),
                MAX_CHANNEL_NAME_LEN - 1);
        sysData->channels[ch].name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
        xSemaphoreGive(*dataMutex);
      }
      nvs->commit();
      consoleAppendf(out, "  Channel %d name reset to \"%s\"",
                     ch + 1, sysData->channels[ch].name);
    } else {
      for (int i = 0; i < NUM_CHANNELS; i++) nvs->clearChannelName(i);
      if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < NUM_CHANNELS; i++) {
          strncpy(sysData->channels[i].name, NVSManager::defaultChannelName(i),
                  MAX_CHANNEL_NAME_LEN - 1);
          sysData->channels[i].name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
        }
        xSemaphoreGive(*dataMutex);
      }
      nvs->commit();
      consoleAppendf(out, "%s", "  All channel names reset to defaults");
    }

  } else if (cmd.startsWith("reset ")) {
    int ch = cmd.substring(6).toInt() - 1;
    if (ch >= 0 && ch < NUM_CHANNELS && limitMgr) {
      limitMgr->resetCounter((uint8_t)ch);
      nvs->commit();
      consoleAppendf(out, "  Ch%d counter reset", ch + 1);
    } else {
      consoleAppendf(out, "  Usage: reset <N> (1-%d)", NUM_CHANNELS);
    }

  } else {
    consoleAppendf(out, "%s", "  Unknown command. Type 'help'.");
  }
}

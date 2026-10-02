#include "console_handler.h"

#include <stdarg.h>

#include "../core/limit_manager.h"
#include "../core/power_calculator.h"
#include "../network/ota_handler.h"
#include "../network/wifi_manager.h"
#include "../ui/buzzer.h"
#include "../ui/status_led.h"
#include "../utils/nvs_manager.h"
#include "../utils/log_gate.h"

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
    else if ((uint8_t)c < 0x20) continue;
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
    statusLed(nullptr), ready(false), pendingDefer(false), ledTestActive(false),
    ledTestPhase(0), ledTestNextStep(0) {}

void ConsoleHandler::begin(NVSManager *nvsRef, PowerCalculator *powerCalcRef,
                           SystemData *sysDataRef, SemaphoreHandle_t *mutexRef,
                           Buzzer *buzzerRef, LimitManager *limitMgrRef,
                           WiFiManager *wifiMgrRef, OTAHandler *otaHandlerRef,
                           StatusLED *statusLedRef) {
  nvs = nvsRef;
  powerCalc = powerCalcRef;
  sysData = sysDataRef;
  dataMutex = mutexRef;
  buzzer = buzzerRef;
  limitMgr = limitMgrRef;
  wifiMgr = wifiMgrRef;
  otaHandler = otaHandlerRef;
  statusLed = statusLedRef;
  ready = (nvs && powerCalc && sysData && dataMutex);
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
    // Toggle the live status stream (eco sleep, viewers, bridge, rollover…).
    // Prints a snapshot when switching ON so one command does both jobs.
    g_statusStream = !g_statusStream;
    consoleAppendf(out, "  Status stream %s", g_statusStream ? "ON" : "OFF");
    if (g_statusStream) cmdStatus(out);

  } else if (cmd == "debug") {
    // Toggle developer diagnostics (NVS saves, [CMD] traces, FB errors…).
    g_debugStream = !g_debugStream;
    consoleAppendf(out, "  Debug stream %s", g_debugStream ? "ON" : "OFF");

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
    if (n >= 1 && n <= (int)NUM_CHANNELS && buzzer) {
      buzzer->ring((uint8_t)n);
      consoleAppendf(out, "  Buzzer ringing %d beeps", n);
    } else {
      consoleAppendf(out, "  Usage: buzz <N> (1-%d beeps)", NUM_CHANNELS);
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
        // Audited: previously this silent write left zero trace in the log.
        if (limitMgr) limitMgr->logEnergyWrite((uint8_t)ch, kwh, "Console inject");
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

  } else if (cmd == "test led") {
    if (ledTestActive) {
      consoleAppendf(out, "%s", "  LED test already running");
    } else if (!statusLed) {
      consoleAppendf(out, "%s", "  StatusLED not available");
    } else {
      pendingDefer = true;
      pendingCmd = cmd;
      pendingOutput = "";
      consoleAppendf(out, "%s", "  LED test started (non-blocking)");
    }

  } else if (cmd.startsWith("rms_samples ")) {
    cmdRmsSamples(cmd.substring(12), out);

  } else if (cmd.startsWith("curr_cal ")) {
    cmdCurrCal(cmd.substring(9), out);

  } else if (cmd.startsWith("auto_zero ")) {
    cmdAutoZero(cmd.substring(10), out);

  } else if (cmd.startsWith("volt_cal ")) {
    cmdVoltCal(cmd.substring(9), out);

  } else if (cmd.startsWith("setwifi")) {
    consoleAppendf(out, "%s", "  setwifi is gone: this board is AP-only.");
    consoleAppendf(out, "%s", "  It never joins a network. Join \"" AP_SSID_DEFAULT
                     "\" from your phone instead.");

  } else if (cmd == "clearwifi") {
    cmdClearWifi(out);

  } else if (cmd == "nvs_debug") {
    if (!nvs) {
      consoleAppendf(out, "%s", "  NVS not available");
    } else {
      pendingDefer = true;
      pendingCmd = cmd;
      pendingOutput = "";
      consoleAppendf(out, "%s", "  NVS debug running (deferred)");
    }

  } else if (cmd == "reboot") {
    consoleAppendf(out, "%s", "  Rebooting... (deferred)");
    pendingDefer = true;
    pendingCmd = cmd;
    pendingOutput = "";

  } else {
    consoleAppendf(out, "%s", "  Unknown command. Type 'help'.");
  }
}

void ConsoleHandler::loop() {
  if (!ledTestActive) return;
  if ((int32_t)(millis() - ledTestNextStep) < 0) return;
  ledTestStep(this);
}

void ConsoleHandler::runDeferred() {
  if (!pendingDefer) return;
  pendingDefer = false;
  String cmd = pendingCmd;
  pendingCmd = "";
  pendingOutput = "";

  if (cmd == "test led") {
    ledTestActive = true;
    ledTestPhase = -1;
    ledTestNextStep = 0;
    consoleAppendf(pendingOutput, "%s", "  LED test: starting");
    return;
  }

  if (cmd == "nvs_debug") {
    cmdNvsDebug(pendingOutput);
  } else if (cmd == "reboot") {
    cmdReboot(pendingOutput);
  }
}

String ConsoleHandler::takePendingOutput() {
  String s = pendingOutput;
  pendingOutput = "";
  return s;
}

void ConsoleHandler::cmdLedTest(String &out) {
  consoleAppendf(out, "%s", "  LED test started");
}

void ConsoleHandler::ledTestStep(ConsoleHandler *self) {
  self->ledTestPhase++;
  const int totalPhases = 11;
  if (self->ledTestPhase >= totalPhases) {
    self->ledTestActive = false;
    self->ledTestPhase = 0;
    consoleAppendf(self->pendingOutput, "%s", "  LED test done");
    self->statusLed->setMode(LED_SOLID_GREEN);
    self->statusLed->loop();
    return;
  }

  switch (self->ledTestPhase) {
    case 0:
      consoleAppendf(self->pendingOutput, "%s", "  LED test: GREEN");
      self->statusLed->setMode(LED_SOLID_GREEN);
      self->statusLed->loop();
      break;
    case 1:
      consoleAppendf(self->pendingOutput, "%s", "  LED test: YELLOW (blink)");
      self->statusLed->setMode(LED_BLINK_YELLOW);
      break;
    case 2:
      self->statusLed->loop();
      break;
    case 3:
      self->statusLed->loop();
      break;
    case 4:
      self->statusLed->loop();
      break;
    case 5:
      consoleAppendf(self->pendingOutput, "%s", "  LED test: RED");
      self->statusLed->setMode(LED_SOLID_RED);
      self->statusLed->loop();
      break;
    case 6:
      consoleAppendf(self->pendingOutput, "%s", "  LED test: RED (blink)");
      self->statusLed->setMode(LED_BLINK_RED);
      break;
    case 7:
      self->statusLed->loop();
      break;
    case 8:
      self->statusLed->loop();
      break;
    case 9:
      self->statusLed->loop();
      break;
    case 10:
      consoleAppendf(self->pendingOutput, "%s", "  LED test: BLUE");
      self->statusLed->setMode(LED_SOLID_BLUE);
      self->statusLed->loop();
      break;
  }
  self->ledTestNextStep = millis() + 500;
}

void ConsoleHandler::cmdNvsDebug(String &out) {
  consoleAppendf(out, "%s", "  NVS Debug (cache read):");
  nvs->saveRmsSamples(888);
  uint16_t rr = nvs->loadRmsSamples();
  consoleAppendf(out, "    Write rms_samp=%d  Read(cache) rms_samp=%d  %s",
                 888, rr, (rr == 888) ? "OK" : "FAIL");
  nvs->saveChannelCurrentCal(0, 7.5f);
  float rc = nvs->loadChannelCurrentCal(0);
  consoleAppendf(out, "    Write ch1_ccal=%.1f  Read(cache) ch1_ccal=%.1f  %s",
                 7.5f, rc, (rc == 7.5f) ? "OK" : "FAIL");
  nvs->saveVoltageCalibration(42.5f);
  float rv = nvs->loadVoltageCalibration();
  consoleAppendf(out, "    Write volt_cal=%.1f  Read(cache) volt_cal=%.1f  %s",
                 42.5f, rv, (rv == 42.5f) ? "OK" : "FAIL");
  nvs->commit();
  rr = nvs->loadRmsSamples();
  rc = nvs->loadChannelCurrentCal(0);
  rv = nvs->loadVoltageCalibration();
  consoleAppendf(out, "%s", "  NVS Debug (flash read after commit):");
  consoleAppendf(out, "    rms_samp=%d  ch1_ccal=%.1f  volt_cal=%.1f",
                 rr, rc, rv);
}

void ConsoleHandler::flushEnergy() {
  if (!nvs || !powerCalc) return;

  // commit() is prefs.end() + prefs.begin(), which is NOT thread-safe, and
  // sensorTask does the same putFloat+commit on this same Preferences handle
  // every ~5 s under dataMutex. With STA gone, `reboot` is now the ONLY
  // restart path in the firmware, so this flush is the last chance to get the
  // counters into flash - it must not race the periodic save.
  //
  // Bounded wait: if sensorTask happens to hold the mutex we proceed anyway
  // rather than skip the flush, because a missed save on a reboot is exactly
  // the data loss this function exists to prevent.
  bool locked = (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE);
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    nvs->saveEnergyKWh(ch, powerCalc->getEnergyKWh(ch));
  }
  nvs->commit();
  if (locked) xSemaphoreGive(*dataMutex);
}

void ConsoleHandler::cmdReboot(String &out) {
  consoleAppendf(out, "%s", "  Rebooting...");
  flushEnergy();
  nvs->end();
  delay(1000);
  ESP.restart();
}

void ConsoleHandler::cmdRmsSamples(const String &args, String &out) {
  int val = args.toInt();
  if (val >= 100 && val <= MAX_RMS_SAMPLES) {
    powerCalc->setRmsSamples(val);
    nvs->saveRmsSamples(val);
    nvs->commit();
    if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      sysData->rmsSamples = val;
      xSemaphoreGive(*dataMutex);
    }
    consoleAppendf(out, "  RMS samples set to %d", val);
  } else {
    consoleAppendf(out, "  RMS samples must be 100-%d", MAX_RMS_SAMPLES);
  }
}

void ConsoleHandler::cmdCurrCal(const String &args, String &out) {
  int sp = args.indexOf(' ');
  if (sp > 0) {
    int ch = args.substring(0, sp).toInt() - 1;
    float val = args.substring(sp + 1).toFloat();
    if (ch >= 0 && ch < NUM_CHANNELS && val > 0) {
      powerCalc->currentCal[ch] = val;
      nvs->saveChannelCurrentCal(ch, val);
      nvs->commit();
      if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->currentCalibration[ch] = val;
        xSemaphoreGive(*dataMutex);
      }
      consoleAppendf(out, "  Ch%d current calibration set to %.1f", ch + 1, val);
    } else {
      consoleAppendf(out, "%s", "  Usage: curr_cal <ch 1-6> <val>");
    }
  } else {
    consoleAppendf(out, "%s", "  Usage: curr_cal <ch 1-6> <val>");
  }
}

void ConsoleHandler::cmdAutoZero(const String &args, String &out) {
  int ch = args.toInt() - 1;
  if (ch >= 0 && ch < NUM_CHANNELS) {
    powerCalc->requestAutoZero(ch);
    consoleAppendf(out, "  Ch%d auto-zero requested (runs on next sensor cycle)", ch + 1);
  } else {
    consoleAppendf(out, "%s", "  Usage: auto_zero <ch 1-6>");
  }
}

void ConsoleHandler::cmdVoltCal(const String &args, String &out) {
  float val = args.toFloat();
  if (val > 0) {
    powerCalc->voltageCal = val;
    nvs->saveVoltageCalibration(val);
    nvs->commit();
    if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      sysData->voltageCalibration = val;
      xSemaphoreGive(*dataMutex);
    }
    consoleAppendf(out, "  Voltage calibration set to %.1f", val);
  } else {
    consoleAppendf(out, "%s", "  Usage: volt_cal <val>");
  }
}

void ConsoleHandler::cmdClearWifi(String &out) {
  // Nothing reads WiFi credentials any more, so this only scrubs leftovers
  // from an older firmware in NVS. It is kept deliberately: it is the one way
  // to make sure an old SSID/password is not still sitting in flash.
  nvs->clearWiFi();
  nvs->saveWiFiMode(0);
  nvs->commit();
  consoleAppendf(out, "%s", "  Stored WiFi credentials cleared");
  consoleAppendf(out, "%s", "  (the board is AP-only and never joins a network)");
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
  consoleAppendf(out, "%s", "    status              Toggle live status stream (+snapshot)");
  consoleAppendf(out, "%s", "    debug               Toggle debug diagnostics stream");
  consoleAppendf(out, "%s", "    ch <N>              Channel details (1-6)");
  consoleAppendf(out, "%s", "    cal                 Show calibration values");
  consoleAppendf(out, "%s", "    info                Firmware & hardware info");
  consoleAppendf(out, "%s", "    wifi                Show WiFi status");
  consoleAppendf(out, "%s", "    buzz <N>            Ring buzzer N beeps (1-6)");
  consoleAppendf(out, "%s", "    inject <ch> <kwh>   Set channel energy (testing)");
  consoleAppendf(out, "%s", "    reset <N>           Reset counter for channel (1-6)");
  consoleAppendf(out, "%s", "    reset_name [N]      Reset channel name(s) to default");
  consoleAppendf(out, "%s", "    ---");
  consoleAppendf(out, "%s", "    test led            LED color sequence test (non-blocking)");
  consoleAppendf(out, "%s", "    rms_samples <N>     Set RMS samples (100-MAX)");
  consoleAppendf(out, "%s", "    curr_cal <ch> <val> Set current calibration for channel");
  consoleAppendf(out, "%s", "    auto_zero <ch>      Auto-zero noise floor for channel");
  consoleAppendf(out, "%s", "    volt_cal <val>      Set voltage calibration");
  consoleAppendf(out, "%s", "    setwifi sta|ap|auto Set WiFi mode");
  consoleAppendf(out, "%s", "    setwifi ssid <name> Set WiFi network name");
  consoleAppendf(out, "%s", "    setwifi pass <pwd>  Set WiFi password");
  consoleAppendf(out, "%s", "    setwifi connect     Save + reboot to connect");
  consoleAppendf(out, "%s", "    clearwifi           Erase WiFi credentials");
  consoleAppendf(out, "%s", "    nvs_debug           Test NVS write/read cycle");
  consoleAppendf(out, "%s", "    reboot              Restart the device");
}

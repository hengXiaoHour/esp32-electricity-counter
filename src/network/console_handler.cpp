#include "console_handler.h"

#include <stdarg.h>

#include "../core/limit_manager.h"
#include "../core/power_calculator.h"
#include "../network/ota_handler.h"
#include "../network/wifi_manager.h"
#include "../network/ap_creds.h"
#include "../network/cloud_cfg.h"
#include "../network/cloud_push.h"
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
                           StatusLED *statusLedRef, CloudPush *cloudRef) {
  nvs = nvsRef;
  powerCalc = powerCalcRef;
  sysData = sysDataRef;
  dataMutex = mutexRef;
  buzzer = buzzerRef;
  limitMgr = limitMgrRef;
  wifiMgr = wifiMgrRef;
  otaHandler = otaHandlerRef;
  statusLed = statusLedRef;
  cloud = cloudRef;
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
      // Order matters: the command BEFORE the flag. loop() on Core 1
      // consumes the flag and reads the command - flag-first lets it grab
      // a stale command and swallow this one silently (2026-10-06: serial
      // `reboot` acked but never reset).
      pendingCmd = cmd;
      pendingDefer = true;
      pendingOutput = "";
      consoleAppendf(out, "%s", "  LED test started (non-blocking)");
    }

  } else if (cmd.startsWith("rms_samples ")) {
    cmdRmsSamples(cmd.substring(12), out);

  } else if (cmd.startsWith("az_batches ")) {
    cmdAzBatches(cmd.substring(11), out);

  } else if (cmd.startsWith("curr_cal ")) {
    cmdCurrCal(cmd.substring(9), out);

  } else if (cmd.startsWith("auto_zero ")) {
    cmdAutoZero(cmd.substring(10), out);

  } else if (cmd.startsWith("volt_cal ")) {
    cmdVoltCal(cmd.substring(9), out);

  } else if (cmd.startsWith("setwifi ")) {
    cmdSetWifi(cmd.substring(8), out);

  } else if (cmd.startsWith("setwifi")) {
    consoleAppendf(out, "%s", "  Usage: setwifi <ssid> <password>");
    consoleAppendf(out, "%s", "  Saves to NVS, then reboots to join that network.");

  } else if (cmd.startsWith("set_ap ")) {
    cmdSetAp(cmd.substring(7), out);

  } else if (cmd == "reset_ap") {
    cmdResetAp(out);

  } else if (cmd.startsWith("reset_day")) {
    cmdResetDay(cmd.length() > 9 ? cmd.substring(9) : "", out);

  } else if (cmd == "clearwifi") {
    cmdClearWifi(out);

  } else if (cmd.startsWith("setcloud ")) {
    cmdSetCloud(cmd.substring(9), out);

  } else if (cmd.startsWith("setcloud")) {
    consoleAppendf(out, "%s", "  Usage: setcloud <db-host> <email> <password>");
    consoleAppendf(out, "%s", "  STA-only remote monitoring; reboots to apply.");

  } else if (cmd == "clearcloud") {
    cmdClearCloud(out);

  } else if (cmd == "cloud diag") {
    cmdCloudDiag(out);

  } else if (cmd == "ota status" || cmd.equalsIgnoreCase("ota status")) {
    if (otaHandler) otaHandler->cloudStatus(out);
    else consoleAppendf(out, "%s", "  OTA not available");

  } else if (cmd == "version") {
    // cloud-ota parity: the compiled stamp, plus the chip so the right
    // release asset can be picked without guessing.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    consoleAppendf(out, "  FW: %s (esp32s3)", FIRMWARE_VERSION);
#else
    consoleAppendf(out, "  FW: %s (esp32)", FIRMWARE_VERSION);
#endif

  } else if (cmd == "update") {
    cmdUpdate(out);

  } else if (cmd.startsWith("ota ")) {
    cmdOta(cmd.substring(4), out);

  } else if (cmd == "ota") {
    consoleAppendf(out, "%s", "  Usage: ota <https://github.com/.../releases/download/.../*.bin>");
    consoleAppendf(out, "%s", "         ota status   (updater state, survives reboot)");
    consoleAppendf(out, "%s", "         update       (check version.json, arms a boot check when runtime TLS can't)");
    consoleAppendf(out, "%s", "         version      (compiled firmware stamp)");

  } else if (cmd == "led" || cmd.startsWith("led ")) {
    cmdLed(cmd.length() > 3 ? cmd.substring(4) : "", out);

  } else if (cmd == "nvs_debug") {
    if (!nvs) {
      consoleAppendf(out, "%s", "  NVS not available");
    } else {
      pendingCmd = cmd;
      pendingDefer = true;
      pendingOutput = "";
      consoleAppendf(out, "%s", "  NVS debug running (deferred)");
    }

  } else if (cmd == "reboot") {
    consoleAppendf(out, "%s", "  Rebooting... (deferred)");
    pendingCmd = cmd;
    pendingDefer = true;
    pendingOutput = "";

  } else if (cmd.startsWith("test_force_rollover")) {
    // Guarded CLI form of the JSON test verb: the bare verb is a dry run
    // that only describes what WOULD happen. Only the exact word CONFIRM
    // arms it — same marker yank + audit as {"cmd":"test_force_rollover"}.
    String args = cmd.length() > 19 ? cmd.substring(19) : "";
    args.trim();
    if (args == "CONFIRM") {
      if (!nvs) {
        consoleAppendf(out, "%s", "  NVS not available");
      } else {
        nvs->saveLastMonth(202607);
        nvs->commit();
        if (limitMgr) limitMgr->auditForceRollover();
        consoleAppendf(out, "%s", "  Rollover test ARMED — next sensor cycle zeroes all counters");
      }
    } else {
      consoleAppendf(out, "%s", "  DRY RUN — would yank the billing marker to 202607 and the");
      consoleAppendf(out, "%s", "  next sensor cycle would zero every counter. Nothing done.");
      consoleAppendf(out, "%s", "  To fire for real: test_force_rollover CONFIRM");
    }

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
  String note = pendingRebootNote;
  pendingRebootNote = "";
  // Core-1 proof of consumption: if the flag is seen but the handler below
  // wedges, this line (not the handler's output) tells us which side died.
  Serial.print("  [deferred] running '");
  Serial.print(cmd);
  Serial.println("'");

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
    if (note.length() > 0) consoleAppendf(pendingOutput, "%s", note.c_str());
    cmdReboot(pendingOutput);
  } else {
    // A consumed flag with no matching command means Core 1 interleaved
    // between the flag and command writes - the old flag-first order did
    // exactly this. Never swallow it silently again.
    consoleAppendf(pendingOutput, "  Lost deferred command: '%s'", cmd.c_str());
  }
}

String ConsoleHandler::takePendingOutput() {
  String s = pendingOutput;
  pendingOutput = "";
  return s;
}

void ConsoleHandler::requestReboot(const char *reason) {
  // Order: command BEFORE the flag (see the exec() setters). loop() on
  // Core 1 consumes the flag first - flag-first loses the command.
  pendingRebootNote = reason ? String(reason) : "";
  pendingCmd = "reboot";
  pendingOutput = "";
  pendingDefer = true;
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
  // No nvs->end() here on purpose. The handle must stay OPEN until the reset:
  // end() makes every read return its default (last_month reads as 0), so the
  // sensor task's rollover check, which keeps running during the delay below,
  // would see a fake fresh-board marker and zero a month of counters before
  // the restart (the 2026-10-05 reboot wipes). ESP.restart() needs nothing
  // closed — flushEnergy's commit already left flash consistent.
  delay(1000);
  ESP.restart();
}

void ConsoleHandler::cmdAzBatches(const String &args, String &out) {
  int val = args.toInt();
  if (val >= 1 && val <= PowerCalculator::AZ_BATCHES_MAX) {
    powerCalc->setAzBatches(val);
    nvs->saveAzBatches((uint8_t)val);
    nvs->commit();
    consoleAppendf(out, "  Auto-zero batches set to %d", val);
  } else {
    consoleAppendf(out, "  Auto-zero batches must be 1-%d",
                   PowerCalculator::AZ_BATCHES_MAX);
  }
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

void ConsoleHandler::cmdLed(const String &args, String &out) {
  String a = args;
  a.trim();
  a.toLowerCase();

  if (a.length() == 0 || a == "status") {
    consoleAppendf(out, "  LED type: %s",
                   statusLed ? (statusLed->isRgb() ? "rgb" : "normal") : "n/a");
    consoleAppendf(out, "%s", "  Change: led <normal|rgb>   (saved, no reboot)");
    return;
  }

  bool wantRgb;
  if (a == "normal" || a == "plain") {
    wantRgb = false;
  } else if (a == "rgb" || a == "ws2812" || a == "neopixel") {
    wantRgb = true;
  } else {
    consoleAppendf(out, "%s", "  Usage: led <normal|rgb>");
    return;
  }

  bool locked = (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE);
  nvs->saveLedType(wantRgb);
  nvs->commit();
  if (locked) xSemaphoreGive(*dataMutex);

  statusLed->setType(wantRgb);
  statusLed->loop();  // apply the new driver to the current mode immediately
  consoleAppendf(out, "  LED type set to \"%s\" (saved to NVS, no reboot)",
                 wantRgb ? "rgb" : "normal");
}

void ConsoleHandler::cmdOta(const String &args, String &out) {
  // Cloud firmware update, STA-only. 3.2.9 reboot-to-updater: the URL is
  // validated, PERSISTED to NVS and the board reboots; the first network
  // tick with STA + clock downloads it BEFORE the cloud SDK sessions exist,
  // against a clean heap (3.2.7: 34 KB largest block cannot handshake at
  // runtime). The deferred reboot flushes the counters first. Safe to call
  // from any console: serial, dashboard, or the cloud downlink (which skips
  // the PIN, trusting the admin-only /cmd rules instead).
  if (!otaHandler) {
    consoleAppendf(out, "%s", "  OTA not available");
    return;
  }
  String url = args;
  url.trim();
  String reply;
  if (!otaHandler->startCloudUpdate(url.c_str(),
                                   wifiMgr ? wifiMgr->stationUp() : false, reply)) {
    consoleAppendf(out, "%s", reply.c_str());
    return;
  }
  // dataMutex: commit() is prefs.end()+prefs.begin() and is not thread-safe
  // against sensorTask's 5 s energy save on the same handle.
  bool locked = (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE);
  nvs->saveOtaPending(url.c_str());
  nvs->clearOtaErr();
  nvs->commit();
  if (locked) xSemaphoreGive(*dataMutex);
  consoleAppendf(out, "%s", reply.c_str());
  requestReboot("  (OTA staged - rebooting into the updater)");
}

void ConsoleHandler::cmdUpdate(String &out) {
  // Two-tier check. Fast path: runtime version.json fetch (works on clean
  // heaps). Fallback: arm a boot check and reboot - the first STA tick then
  // runs the fetch on the clean heap, where even a 34 KB-max board succeeds.
  // Classic boards almost always take the fallback; that is expected, not a
  // failure. No web button yet - serial only.
  if (!otaHandler) {
    consoleAppendf(out, "%s", "  OTA not available");
    return;
  }
  String reply, stageUrl;
  OTAHandler::CheckOutcome oc = otaHandler->checkForUpdate(
      wifiMgr ? wifiMgr->stationUp() : false, true, reply, stageUrl);
  if (oc == OTAHandler::CHECK_STAGED) {
    bool locked = (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE);
    nvs->saveOtaPending(stageUrl.c_str());
    nvs->clearOtaErr();
    nvs->clearOtaMsg();
    nvs->commit();
    if (locked) xSemaphoreGive(*dataMutex);
    consoleAppendf(out, "%s", reply.c_str());
    requestReboot("  (update staged - rebooting into the updater)");
    return;
  }
  if (oc != OTAHandler::CHECK_RETRYABLE) {
    consoleAppendf(out, "%s", reply.c_str());
    return;
  }
  // Runtime link blocked (DNS/TLS/heap): persist the intent and reboot into
  // the clean heap. The outcome lands in `ota status` after boot.
  bool locked = (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE);
  nvs->saveOtaCheck();
  nvs->commit();
  if (locked) xSemaphoreGive(*dataMutex);
  consoleAppendf(out, "%s", reply.c_str());
  consoleAppendf(out, "%s", "  (check armed - rebooting, `ota status` after boot)");
  requestReboot("  (update armed - rebooting into the clean-heap check)");
}

void ConsoleHandler::cmdClearWifi(String &out) {
  // Nothing reads WiFi credentials any more, so this only scrubs leftovers
  // from an older firmware in NVS. It is kept deliberately: it is the one way
  // to make sure an old SSID/password is not still sitting in flash.
  nvs->clearWiFi();
  nvs->commit();
  consoleAppendf(out, "%s", "  Stored WiFi credentials cleared");
  consoleAppendf(out, "%s", "  (on next boot the board will not join any network)");
}

void ConsoleHandler::cmdSetWifi(const String &args, String &out) {
  // Same parsing helper as set_ap, so `setwifi "My Router" pass123` works and
  // an over-long value is REFUSED rather than truncated into a different SSID.
  char ssidBuf[AP_MAX_SSID_LEN + 1];
  char passBuf[AP_MAX_PASS_LEN + 1];
  if (!ap_creds_splitArgs(args.c_str(), ssidBuf, sizeof(ssidBuf),
                          passBuf, sizeof(passBuf))) {
    consoleAppendf(out, "%s", "  Usage: setwifi <ssid> <password>");
    consoleAppendf(out, "%s", "         quote an SSID that contains spaces:");
    consoleAppendf(out, "%s", "         setwifi \"Home Router\" mypass123");
    return;
  }

  if (strlen(ssidBuf) == 0) {
    consoleAppendf(out, "%s", "  Not saved: the SSID is empty.");
    return;
  }

  // dataMutex for the same reason set_ap takes it: commit() is
  // prefs.end()+prefs.begin() and is not thread-safe against sensorTask.
  bool locked = (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE);
  nvs->saveWiFi(ssidBuf, passBuf);
  nvs->commit();
  if (locked) xSemaphoreGive(*dataMutex);

  consoleAppendf(out, "  Saved. The board will join \"%s\" on boot.", ssidBuf);
  consoleAppendf(out, "%s",
                 "  It still broadcasts its own AP, so the phone dashboard keeps working.");
  requestReboot("  (station WiFi changed - restarting)");
}

void ConsoleHandler::cmdClearCloud(String &out) {
  bool locked = (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE);
  nvs->clearFb();
  nvs->commit();
  if (locked) xSemaphoreGive(*dataMutex);
  consoleAppendf(out, "%s", "  Remote monitoring stopped (cleared, no reboot)");
}

void ConsoleHandler::cmdCloudDiag(String &out) {
  // Read-only session readout: proves WHAT token the board holds (length +
  // audience claim) without ever printing it. No PIN: like `wifi` and
  // `status`, this reveals no secret - account/host/MAC are dashboard-visible
  // already, and length+aud cannot reconstruct a token.
  if (!cloud) {
    consoleAppendf(out, "%s", "  Cloud not wired (restart the board)");
    return;
  }
  cloud->diag(out, dataMutex);
}

void ConsoleHandler::cmdSetCloud(const String &args, String &out) {
  // Three fields, so the two-splitter (ap_creds_splitArgs) does not fit: the
  // password may contain spaces, so it is the REMAINDER of the line. Shape
  // rules are cloud_validate*, host-tested on the PC.
  char hostBuf[CLOUD_MAX_HOST_LEN + 1];
  char emailBuf[CLOUD_MAX_EMAIL_LEN + 1];
  char passBuf[CLOUD_MAX_PASS_LEN + 1];
  if (!cloud_splitArgs3(args.c_str(), hostBuf, sizeof(hostBuf),
                        emailBuf, sizeof(emailBuf), passBuf, sizeof(passBuf))) {
    consoleAppendf(out, "%s", "  Usage: setcloud <db-host> <email> <password>");
    consoleAppendf(out, "%s", "         host only, no https://, no path - e.g.");
    consoleAppendf(out, "%s", "         setcloud my-proj.firebaseio.com board@esp32.local SECRET");
    return;
  }

  if (!cloud_validateHost(hostBuf)) {
    consoleAppendf(out, "%s", "  Not saved: bad database host (host only, no https://, no path).");
    return;
  }
  if (!cloud_validateEmail(emailBuf)) {
    consoleAppendf(out, "%s", "  Not saved: bad account email.");
    return;
  }
  if (!cloud_validatePass(passBuf)) {
    consoleAppendf(out, "%s", "  Not saved: bad account password.");
    return;
  }

  // dataMutex for the same reason setwifi takes it: commit() is
  // prefs.end()+prefs.begin() and is not thread-safe against sensorTask.
  bool locked = (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE);
  nvs->saveFb(hostBuf, emailBuf, passBuf);
  nvs->commit();
  if (locked) xSemaphoreGive(*dataMutex);

  consoleAppendf(out, "  Saved. Signing in as \"%s\" on boot (STA only).", emailBuf);
  consoleAppendf(out, "%s", "  The password is never shown back - not here, not on the dashboard.");
  requestReboot("  (cloud monitoring changed - restarting)");
}

void ConsoleHandler::cmdSetAp(const String &args, String &out) {
  // Parsing (including the quoted "name with spaces" form) lives in ap_creds so
  // it can be unit-tested on the host; see ap_creds_splitArgs().
  char ssidBuf[AP_MAX_SSID_LEN + 1];
  char passBuf[AP_MAX_PASS_LEN + 1];
  if (!ap_creds_splitArgs(args.c_str(), ssidBuf, sizeof(ssidBuf),
                          passBuf, sizeof(passBuf))) {
    consoleAppendf(out, "%s", "  Usage: set_ap <name> <password>");
    consoleAppendf(out, "%s", "         quote a name that contains spaces:");
    consoleAppendf(out, "%s", "         set_ap \"Living Room Meter\" mypass123");
    return;
  }

  // The one rule that matters: nothing reaches flash until it is a pair the
  // radio can actually broadcast. A short PSK makes softAP() fail, and the board
  // would come back with no network at all - recoverable only over serial.
  const char *reason = nullptr;
  if (!ap_creds_validate(ssidBuf, passBuf, &reason)) {
    consoleAppendf(out, "  Not saved: %s", reason ? reason : "invalid credentials");
    consoleAppendf(out, "%s", "  Name 1-32 chars; password 8-63 chars.");
    return;
  }

  // dataMutex: commit() is prefs.end()+prefs.begin() and is not thread-safe
  // against sensorTask's 5 s energy save, which takes the same handle under
  // this same mutex.
  bool locked = (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE);
  nvs->saveApCredentials(ssidBuf, passBuf);
  nvs->commit();
  if (locked) xSemaphoreGive(*dataMutex);

  consoleAppendf(out, "  Saved. New network name: \"%s\"", ssidBuf);
  consoleAppendf(out, "%s",
                 "  The board reboots now - join that network and reopen the page.");
  requestReboot("  (AP credentials changed - restarting)");
}

void ConsoleHandler::cmdResetAp(String &out) {
  bool locked = (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE);
  nvs->clearApCredentials();
  nvs->commit();
  if (locked) xSemaphoreGive(*dataMutex);

  consoleAppendf(out, "  AP name and password reset to \"%s\" / \"%s\"",
                 AP_SSID_DEFAULT, AP_PASS_DEFAULT);
  consoleAppendf(out, "%s", "  The board reboots now - rejoin that network.");
  requestReboot("  (AP credentials reset - restarting)");
}

void ConsoleHandler::cmdResetDay(const String &args, String &out) {
  int day = args.toInt();
  if (day >= 1 && day <= 28 && limitMgr) {
    if (limitMgr->setResetDay((uint8_t)day)) {
      consoleAppendf(out, "  Billing reset day set to %d — counters zero at 00:00 local time (UTC+7) on day %d each month (NVS level)", day, day);
    } else {
      consoleAppendf(out, "%s", "  Could not take the data lock - retry");
    }
  } else {
    consoleAppendf(out, "  Usage: reset_day <1-28>%s",
                   limitMgr ? "" : " (unavailable)");
  }
}

void ConsoleHandler::cmdStatus(String &out) {
  uint32_t up = millis() / 1000;
  consoleAppendf(out, "  %-16s%02lu:%02lu:%02lu", "Uptime:",
                 (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60),
                 (unsigned long)(up % 60));
  consoleAppendf(out, "  %-16s%s  (%d client(s))", "Network:",
                 (wifiMgr && wifiMgr->isReady()) ? "AP MODE" : "STARTING",
                 wifiMgr ? (int)wifiMgr->clientCount() : 0);
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
  // Task diagnostics: a wedged Core-1 loop shows up here as a frozen iter
  // count or a dead stack watermark instead of a silent mystery.
  TaskHandle_t lt = xTaskGetHandle("loopTask");
  consoleAppendf(out, "  %-16s%u iters", "Loop:", (unsigned)g_loopIters);
  consoleAppendf(out, "  %-16sloop=%u net=%u sen=%u words free", "Stacks:",
                 lt ? (unsigned)uxTaskGetStackHighWaterMark(lt) : 0,
                 g_networkTask ? (unsigned)uxTaskGetStackHighWaterMark(g_networkTask) : 0,
                 g_sensorTask ? (unsigned)uxTaskGetStackHighWaterMark(g_sensorTask) : 0);
}

void ConsoleHandler::cmdWifi(String &out) {
  // STA is the default path; the AP below is fallback-only and usually OFF.
  const bool up = wifiMgr && wifiMgr->stationUp();
  const bool ap = wifiMgr && wifiMgr->apActive();
  consoleAppendf(out, "  %-16s%s", "Mode:",
                 ap ? "AP FALLBACK (home network failed)"
                    : (up ? "STA (AP off)" : "STA joining..."));
  consoleAppendf(out, "%s", "");
  consoleAppendf(out, "  Home network:");
  consoleAppendf(out, "  %-16s%s", "SSID:",
                 up ? WiFi.SSID().c_str() : (wifiMgr ? wifiMgr->staSSID() : ""));
  if (up) {
    consoleAppendf(out, "  %-16s%s", "Status:", "CONNECTED");
    consoleAppendf(out, "  %-16s%d dBm", "RSSI:", (int)wifiMgr->staRSSI());
    consoleAppendf(out, "  %-16s%s", "IP:", WiFi.localIP().toString().c_str());
    consoleAppendf(out, "  %-16shttp://%s/", "Dashboard:", WiFi.localIP().toString().c_str());
  } else {
    consoleAppendf(out, "  %-16s%s", "Status:", "not connected");
    consoleAppendf(out, "%s", "  Change it: setwifi <ssid> <password>  (reboots)");
    consoleAppendf(out, "%s", "  Forget it: clearwifi");
  }

  consoleAppendf(out, "%s", "");
  if (ap) {
    consoleAppendf(out, "  Fallback AP: ON");
    consoleAppendf(out, "  %-16s\"%s\"", "Network:",
                   wifiMgr ? wifiMgr->getSSID() : AP_SSID_DEFAULT);
    consoleAppendf(out, "  %-16s%s", "AP IP:", WiFi.softAPIP().toString().c_str());
    consoleAppendf(out, "  %-16s%s", "Dashboard:", "http://192.168.4.1/");
    consoleAppendf(out, "  %-16s%d", "Clients:", (int)wifiMgr->clientCount());
    consoleAppendf(out, "%s", "  Change it: set_ap <name> <password>   (reboots)");
    consoleAppendf(out, "%s", "  Forgot it? reset_ap                    (back to defaults)");
  } else {
    consoleAppendf(out, "%s", "  Fallback AP: OFF (home network is up)");
  }
}

void ConsoleHandler::cmdHelp(String &out) {
  consoleAppendf(out, "%s", "  Commands:");
  consoleAppendf(out, "%s", "    help                Show available commands");
  consoleAppendf(out, "%s", "    status              Toggle live status stream (+snapshot)");
  consoleAppendf(out, "%s", "    debug               Toggle debug diagnostics stream");
  consoleAppendf(out, "%s", "    ch <N>              Channel details (1-5)");
  consoleAppendf(out, "%s", "    cal                 Show calibration values");
  consoleAppendf(out, "%s", "    info                Firmware & hardware info");
  consoleAppendf(out, "%s", "    wifi                Show WiFi status");
  consoleAppendf(out, "%s", "    buzz <N>            Ring buzzer N beeps (1-5)");
  consoleAppendf(out, "%s", "    inject <ch> <kwh>   Set channel energy (testing)");
  consoleAppendf(out, "%s", "    reset <N>           Reset counter for channel (1-5)");
  consoleAppendf(out, "%s", "    reset_name [N]      Reset channel name(s) to default");
  consoleAppendf(out, "%s", "    ---");
  consoleAppendf(out, "%s", "    test led            LED color sequence test (non-blocking)");
  consoleAppendf(out, "%s", "    led <normal|rgb>    LED driver type (default: normal, no reboot)");
  consoleAppendf(out, "%s", "    rms_samples <N>     Set RMS samples (100-MAX)");
  consoleAppendf(out, "%s", "    az_batches <N>      Set auto-zero captures (1-64)");
  consoleAppendf(out, "%s", "    curr_cal <ch> <val> Set current calibration for channel");
  consoleAppendf(out, "%s", "    auto_zero <ch>      Auto-zero noise floor for channel");
  consoleAppendf(out, "%s", "    volt_cal <val>      Set voltage calibration");
  consoleAppendf(out, "%s", "    setwifi <ssid> <pw> Home network to join on boot (reboots)");
  consoleAppendf(out, "%s", "    clearwifi           Erase the saved home-network creds");
  consoleAppendf(out, "%s", "    setcloud <host> <email> <pass> Sign in + push to realtime DB (reboots)");
  consoleAppendf(out, "%s", "    cloud diag            Show cloud session (account, token age/length/aud)");
  consoleAppendf(out, "%s", "    ota <url>             Download + flash a github release .bin (STA only, reboots)");
  consoleAppendf(out, "%s", "    ota status            Updater state, survives reboot");
  consoleAppendf(out, "%s", "    update                Check version.json (reboots to the clean-heap check when runtime TLS can't)");
  consoleAppendf(out, "%s", "    version               Compiled firmware stamp + chip");
  consoleAppendf(out, "%s", "    clearcloud          Stop remote monitoring (no reboot)");
  consoleAppendf(out, "%s", "    set_ap <name> <pw>  Rename the network + set password (reboots)");
  consoleAppendf(out, "%s", "    reset_ap            Restore the default network name (reboots)");
  consoleAppendf(out, "%s", "    reset_day <1-28>    Billing reset day (counters zero at 00:00 local, UTC+7)");
  consoleAppendf(out, "%s", "    nvs_debug           Test NVS write/read cycle");
  consoleAppendf(out, "%s", "    test_force_rollover CONFIRM  Arm a billing wipe for real (bare = dry run)");
  consoleAppendf(out, "%s", "    reboot              Restart the device");
}

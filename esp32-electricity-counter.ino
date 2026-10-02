#include "src/config.h"
#include "src/core/power_calculator.h"
#include "src/core/limit_manager.h"

#include "src/network/wifi_manager.h"
#include "src/network/websocket_server.h"
#include "src/network/firebase_bridge.h"
#include "src/network/ota_handler.h"
#include "src/network/ntfy_notifier.h"
#include "src/network/console_handler.h"
#include "src/ui/status_led.h"  
#include "src/ui/buzzer.h"
#include "src/utils/nvs_manager.h"
#include "src/utils/log_gate.h"

#include <esp_bt.h>

NVSManager      nvs;
PowerCalculator powerCalc;
Buzzer          buzzer;
LimitManager    limitMgr;
NtfyNotifier    ntfyNotifier;

WiFiManager     wifiMgr;
WebSocketServer wsServer;
FirebaseBridge  fbBridge;
OTAHandler      otaHandler;
StatusLED       statusLED;

SystemData systemData;
SemaphoreHandle_t dataMutex;

static uint32_t lastSensorCycle = 0;

// Auto-zero is processed in small chunks per sensor cycle so the sensing loop
// (update + broadcast) never stalls while a channel is being captured. More
// batches per cycle = faster, but longer single-cycle hold.
static const int AZ_BATCHES_PER_CYCLE = 2;

void onWiFiEvent(WiFiEvent_t event, arduino_event_info_t info) {
  // Only AP events matter now. The old STA_GOT_IP hook existed to gate a
  // one-shot banner print; that banner waits on wifiMgr.isReady() instead,
  // which is a stronger signal than the event (softAP() has returned and the
  // interface actually holds 192.168.4.1).
  if (event == ARDUINO_EVENT_WIFI_AP_START) {
    STATUS_LOG("  [WiFi] soft AP started\n");
  }
}

// Forward declarations
static void handleSerialCommand(const String &cmd);

static void updateLED() {
  if (otaHandler.isInProgress()) {
    statusLED.setMode(LED_SOLID_BLUE);
    statusLED.loop();
    return;
  }

  if (!wifiMgr.isReady()) {
    statusLED.setMode(LED_SOLID_RED);
    statusLED.loop();
    return;
  }

  statusLED.setMode(LED_SOLID_GREEN);
  statusLED.loop();
}

static void updateSharedData() {
  systemData.voltageRMS = powerCalc.getVoltageRMS();
  systemData.voltageCalibration = powerCalc.voltageCal;
  systemData.rmsSamples = powerCalc.rmsSamples;
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    systemData.currentCalibration[ch] = powerCalc.currentCal[ch];
  }
  systemData.uptime = millis() / 1000;
  // AP-only: the board is never a station client, so there is no link to
  // report. apMode carries the single meaningful bit; the wifi fields stay in
  // SystemData so the snapshot shape does not change under the dashboard.
  systemData.wifiConnected = false;
  systemData.wifiRSSI = 0;
  systemData.apMode = wifiMgr.isReady();
  systemData.otaInProgress = otaHandler.isInProgress();
  systemData.otaProgress = otaHandler.getProgress();

  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    systemData.channels[ch].currentRMS = powerCalc.getCurrentRMS(ch);
    systemData.channels[ch].activePower = powerCalc.getActivePower(ch);
    systemData.channels[ch].apparentPower = powerCalc.getApparentPower(ch);
    systemData.channels[ch].powerFactor = powerCalc.getPowerFactor(ch);
    systemData.channels[ch].energyKWh = powerCalc.getEnergyKWh(ch);
  }
}

// Push RAM energy counters to NVS + commit before a restart.
// Was duplicated here as WiFiManager's pre-restart hook for the STA
// connect-timeout reboot. That reboot path is gone in AP-only mode, so the
// single remaining flush lives in ConsoleHandler::flushEnergy(), which the
// `reboot` verb calls - and which now takes dataMutex for the same reason
// this one did (NVSManager::commit() is prefs.end()+prefs.begin(), which is
// not thread-safe against sensorTask's 5 s save).

// ==============================
// FreeRTOS Tasks
// ==============================

void networkTask(void *pvParameters) {
  TickType_t lastWake = xTaskGetTickCount();
  char serBuf[64];
  uint8_t serPos = 0;

  while (true) {
    wifiMgr.loop();
    wsServer.loop();
    ntfyNotifier.loop();
    otaHandler.loop();

    // Serial processing on Core 0
    while (Serial.available()) {
      char c = Serial.read();
      if (c >= 32 && c <= 126) Serial.write(c);
      else if (c == '\r') Serial.write('\n');
      if (c == '\n' || c == '\r') {
        if (serPos > 0) {
          serBuf[serPos] = '\0';
          Serial.println();
          handleSerialCommand(String(serBuf));
          serPos = 0;
          Serial.print("> ");
        }
      } else if (serPos < sizeof(serBuf) - 1) {
        serBuf[serPos++] = c;
      }
    }

    // Print the join instructions once the AP is up.
    {
      static bool wifiPrinted = false;
      if (!wifiPrinted && wifiMgr.isReady()) {
        wifiPrinted = true;
        Serial.printf("\n  %-19s%s\n", "WiFi", "AP MODE (always)");
        Serial.printf("  %-19s\"%s\"\n", "Network:", WiFiManager::AP_SSID);
        Serial.printf("  %-19s%s\n", "Password:", WiFiManager::AP_PASS);
        Serial.printf("  %-19shttp://%s/\n", "Dashboard:", WiFi.softAPIP().toString().c_str());
      }
    }

    // The dashboard server starts as soon as the AP has an IP. In AP-only mode
    // there is no upstream to wait for, so this is no longer a "wait for
    // WiFi then hope" handshake - softAP() returning IS the readiness signal.
    if (!wsServer.isRunning() && wifiMgr.isReady()) {
      wsServer.startServer();
      Serial.printf("  %-19s%s\n", "Dashboard", "SERVED FROM FLASH");
      Serial.printf("  %-19s%s\n", "WebSocket", "STARTED");
      // ArduinoOTA must start AFTER the interface has an IP — begin() before
      // that leaves it deaf.
      otaHandler.begin("esp32-elec-counter");
      Serial.printf("  %-19s%s\n", "OTA", "STARTED");
      Serial.println();
      Serial.println("  Join the network, then open http://192.168.4.1/");
      Serial.println("  Core 0: Network (AP, WebSocket, Dashboard, OTA)");
      Serial.println("  Core 1: Sensor (ADC, Power, Limits)");
      Serial.println("  Type 'help' for commands");
      Serial.print("> ");
    }

    // Live `status` / `debug` streams. There is no eco decision left to make
    // here: the old "sleep the modem when nobody is watching" logic only ever
    // applied to a STA link, and an access point has to keep beaconing whether
    // or not anyone is connected. Sensing on Core 1 is unaffected either way.
    {
      static uint32_t lastStatusPrint = 0;
      static uint32_t lastDebugPrint = 0;
      if (g_statusStream && millis() - lastStatusPrint >= 2000) {
        lastStatusPrint = millis();
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          Serial.printf("[status] up %lus | AP clients %d | ws %d | W [%.0f %.0f %.0f %.0f %.0f %.0f]\n",
            (unsigned long)(millis() / 1000), wifiMgr.clientCount(),
            (int)wsServer.clientCount(),
            systemData.channels[0].activePower, systemData.channels[1].activePower,
            systemData.channels[2].activePower, systemData.channels[3].activePower,
            systemData.channels[4].activePower, systemData.channels[5].activePower);
          xSemaphoreGive(dataMutex);
        }
      }
      if (g_debugStream && millis() - lastDebugPrint >= 5000) {
        lastDebugPrint = millis();
        Serial.printf("[debug] heap %u | ap %d | ws %d | ota %s | net AP | up %lus\n",
          (unsigned)ESP.getFreeHeap(), wifiMgr.clientCount(),
          (int)wsServer.clientCount(),
          otaHandler.isInProgress() ? "ACTIVE" : "idle",
          (unsigned long)(millis() / 1000));
      }
    }

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      wsServer.broadcastData(systemData);
      xSemaphoreGive(dataMutex);
    }

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(20));
  }
}

// Firebase bridge runs on its own Core 0 task. Its HTTPS calls are blocking,
// so keeping them here (never in networkTask) protects the 150 ms WebSocket
// broadcast cadence — otherwise a slow /latest push or command poll stalls LAN.
// pushLatest()/loop() are throttled internally to FIREBASE_*_INTERVAL_MS.
void firebaseTask(void *pvParameters) {
  TickType_t lastWake = xTaskGetTickCount();
  static bool fbStarted = false;
  while (true) {
    // Start Firebase here — the blocking TLS handshake + token exchange
    // (Firebase.begin) takes 10-20s and must never run in networkTask
    // (would starve IDLE0 and trigger WDT).
    if (!fbStarted && wifiMgr.isConnected()) {
      fbStarted = true;
      fbBridge.start();
    }
    fbBridge.loop();
    fbBridge.pushLatest();
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(50));
  }
}

void sensorTask(void *pvParameters) {
  TickType_t lastWake = xTaskGetTickCount();

  while (true) {
    float deltaSeconds = (millis() - lastSensorCycle) / 1000.0f;
    if (deltaSeconds < 0.001f) deltaSeconds = 0.1f;
    lastSensorCycle = millis();

    if (powerCalc.isAutoZeroBusy()) {
      if (powerCalc.autoZeroStart()) {
        powerCalc.autoZeroCapture(AZ_BATCHES_PER_CYCLE);
        if (powerCalc.autoZeroDone()) {
          int ch = powerCalc.getAutoZeroChannel();
          float median = powerCalc.autoZeroFinish();
          powerCalc.setNoiseFloor(ch, median);
          nvs.saveNoiseFloor(ch, median);
          nvs.commit();
          STATUS_LOG("  [NVS] ch%d auto-zero complete (median of %d): %.3f A\n",
            ch + 1, PowerCalculator::AZ_BATCHES, median);
        }
      }
    }

    powerCalc.update(deltaSeconds);

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      updateSharedData();

      // Persist energy to NVS every ~5s (60 cycles × 80ms).
      // commit() is REQUIRED: saveEnergyKWh only stages a putFloat in the
      // Preferences handle, so without it the counters never reach flash and a
      // restart silently reverts them to the last command's commit point.
      static uint32_t lastEnergySave = 0;
      if (millis() - lastEnergySave > 5000) {
        lastEnergySave = millis();
        for (int ch = 0; ch < NUM_CHANNELS; ch++) {
          nvs.saveEnergyKWh(ch, powerCalc.getEnergyKWh(ch));
        }
        nvs.commit();
      }

      xSemaphoreGive(dataMutex);
    }

    limitMgr.loop();
    buzzer.loop();

    updateLED();
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SENSOR_CYCLE_INTERVAL_MS));
  }
}

// ==============================
// Arduino Entry Points
// ==============================

void setup() {
  Serial.begin(115200);

  // Wait for USB CDC serial to enumerate (up to 3s)
  // Prevents startup banner from being lost on ESP32-S3 USB CDC
  unsigned long serialTimeout = millis() + 3000;
  while (!Serial && millis() < serialTimeout) {
    delay(10);
  }

  // Bluetooth is never used on this board — keep the controller shut down
  // to cut idle draw and reclaim its RAM. Safe even if never enabled.
  btStop();
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);

  DEBUG_LOG("\n");
  DEBUG_LOG("  =============================================\n");
  DEBUG_LOG("   ESP32-S3 6-Channel Electricity Counter\n");
  DEBUG_LOG("  =============================================\n");
  DEBUG_LOG("  CPU: %d MHz  |  Flash: %d MB  |  PSRAM: %s\n",
    getCpuFrequencyMhz(), ESP.getFlashChipSize() / (1024*1024),
    psramFound() ? "OK" : "N/A");
  Serial.println();

  DEBUG_LOG("  %-19s%s\n", "NVS", "OK"); nvs.begin();

  // Restore the forensic event tail saved to flash before the last reboot
  // (RAM log is wiped on restart; Firebase is unreachable while WiFi is
  // down). Restored entries ride the normal Firebase push on reconnect.
  {
    uint8_t n = nvs.loadForensicEvents(systemData.events, EVENT_LOG_SIZE);
    systemData.eventCount = n;
    if (n > 0) {
      STATUS_LOG("  [NVS] restored %d forensic event(s) from flash\n", n);
    }
  }

  // === Phase 2: Init hardware with defaults first ===
  DEBUG_LOG("  %-19s%s\n", "Status LED", "OK"); statusLED.begin();
  DEBUG_LOG("  %-19s%s\n", "Power Calculator", "OK"); powerCalc.begin();
  DEBUG_LOG("  %-19s%s\n", "Buzzer", "OK"); buzzer.begin(PIN_BUZZER);
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    powerCalc.setEnergyKWh(ch, nvs.loadEnergyKWh(ch));
  }

  // === Phase 3: Load ALL persisted data (overrides defaults) ===
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    powerCalc.currentCal[ch] = nvs.loadChannelCurrentCal(ch);
    powerCalc.noiseFloor[ch] = nvs.loadNoiseFloor(ch);
    powerCalc.lpfAlpha[ch] = nvs.loadLpfAlpha(ch);
  }
  powerCalc.rmsSamples = nvs.loadRmsSamples();
  powerCalc.voltageCal = nvs.loadVoltageCalibration();

  DEBUG_LOG("  %-19svoltage=%.1fV  current=%.1f (ch1)  RMS samples=%d\n",
    "Calibration", powerCalc.voltageCal, powerCalc.currentCal[0], powerCalc.rmsSamples);
  DEBUG_LOG("  %-19sok\n", "Noise Floor + LPF");
  DEBUG_LOG("  %s\n", "Channel Config");
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    char name[MAX_CHANNEL_NAME_LEN];
    if (!nvs.loadChannelName(ch, name, sizeof(name))) {
      strncpy(name, NVSManager::defaultChannelName(ch), MAX_CHANNEL_NAME_LEN - 1);
      name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
    }
    strncpy(systemData.channels[ch].name, name, MAX_CHANNEL_NAME_LEN - 1);
    systemData.channels[ch].name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
    systemData.channels[ch].monthlyKwhLimit = nvs.loadMonthlyKwhLimit(ch);
    systemData.channels[ch].status = STATUS_OK;
    DEBUG_LOG("    Ch%d  %-16s  %.1fkWh/mo\n",
      ch+1, name, systemData.channels[ch].monthlyKwhLimit);
  }

  dataMutex = xSemaphoreCreateMutex();
  DEBUG_LOG("  %-19s%s\n", "Mutex", "OK");

  ntfyNotifier.begin(nvs.loadNtfyEnabled(), nvs.loadNtfyTopic());
  limitMgr.begin(nvs, powerCalc, &systemData, &dataMutex, &ntfyNotifier, &buzzer);

  // Shared text-command engine: drives both the serial console and the web
  // UI console (via processCommand -> WebSocket / Firebase).
  // Blocking commands (test led, nvs_debug, reboot) are deferred and run from
  // loop() on Core 1 where blocking is safe (no WDT, no network stall).
  consoleHandler.begin(&nvs, &powerCalc, &systemData, &dataMutex, &buzzer,
                       &limitMgr, &wifiMgr, &otaHandler, &statusLED);

  WiFi.onEvent(onWiFiEvent);

  wsServer.begin(nvs, &systemData, &dataMutex, &powerCalc, &limitMgr, &wifiMgr);
  fbBridge.begin(nvs, &systemData, &dataMutex, &powerCalc, &limitMgr);

   wifiMgr.begin();
  statusLED.setMode(LED_SOLID_RED);
  DEBUG_LOG("  %-19sAP @ %s (always)\n", "WiFi", WiFi.softAPIP().toString().c_str());
  DEBUG_LOG("  %-19s\"%s\" / \"%s\"\n", "Network", WiFiManager::AP_SSID, WiFiManager::AP_PASS);

  // otaHandler.begin() runs deferred from networkTask once WiFi is up
  // (ArduinoOTA started pre-connect never listens). See serverStarted block.
  DEBUG_LOG("  %-19s%s\n", "OTA", "READY");

  // Firmware helper tasks. networkTask keeps priority 2 so its WebSocket
  // broadcast always preempts firebaseTask (priority 1). Firebase's blocking
  // HTTPS/TLS work must never delay the 150 ms broadcast loop on Core 0.
  xTaskCreatePinnedToCore(networkTask, "network", 8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(sensorTask, "sensor", 8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(firebaseTask, "firebase", 32768, NULL, 1, NULL, 0);

  Serial.println();
}

// ==============================
// Serial Command Handler (testing)
// ==============================
static void handleSerialCommand(const String &cmd) {
  if (cmd == "test led") {
    // Blocking LED test — only safe from serial (runs in loop() on Core 1).
    // The web console uses the non-blocking deferred path via consoleHandler.
    Serial.println("  LED test: GREEN"); statusLED.setMode(LED_SOLID_GREEN); statusLED.loop(); delay(1500);
    Serial.println("  LED test: YELLOW (blink)"); statusLED.setMode(LED_BLINK_YELLOW); for (int i = 0; i < 6; i++) { statusLED.loop(); delay(400); }
    Serial.println("  LED test: RED"); statusLED.setMode(LED_SOLID_RED); statusLED.loop(); delay(1500);
    Serial.println("  LED test: RED (blink)"); statusLED.setMode(LED_BLINK_RED); for (int i = 0; i < 6; i++) { statusLED.loop(); delay(300); }
    Serial.println("  LED test: BLUE"); statusLED.setMode(LED_SOLID_BLUE); statusLED.loop(); delay(1500);
    statusLED.setMode(LED_SOLID_GREEN); statusLED.loop();
    Serial.println("  LED test done");
  }
  else {
    // All other commands go through the shared ConsoleHandler so the serial
    // port and the web console stay in lockstep. Blocking commands (nvs_debug,
    // reboot) are deferred and run from loop().
    String out;
    consoleHandler.exec(cmd, out);
    Serial.println();
    Serial.print(out);
    Serial.println();
  }
}

void loop() {
  // Drive the non-blocking LED test state machine + deferred console commands.
  consoleHandler.runDeferred();
  consoleHandler.loop();
  if (consoleHandler.isLedTestRunning() || consoleHandler.hasDeferred()) {
    String out = consoleHandler.takePendingOutput();
    if (out.length() > 0) {
      Serial.print(out);
    }
  }
}

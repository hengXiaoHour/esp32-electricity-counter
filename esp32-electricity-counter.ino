#include "src/config.h"
#include "src/core/power_calculator.h"
#include "src/core/limit_manager.h"

#include "src/network/wifi_manager.h"
#include "src/network/websocket_server.h"
#include "src/network/firebase_bridge.h"
#include "src/network/cloud_ota.h"
#include "src/network/ota_handler.h"
#include "src/network/ntfy_notifier.h"
#include "src/network/console_handler.h"
#include "src/ui/status_led.h"  
#include "src/ui/buzzer.h"
#include "src/utils/nvs_manager.h"

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
static bool wifiIpPrinted = false;

void onWiFiEvent(WiFiEvent_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    wifiIpPrinted = true;
  }
  if (event == ARDUINO_EVENT_WIFI_AP_START) {
    wifiIpPrinted = true;
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

  if (!wifiMgr.isConnected()) {
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
  systemData.wifiConnected = wifiMgr.isConnected();
  systemData.wifiRSSI = wifiMgr.isConnected() ? wifiMgr.getRSSI() : 0;
  systemData.apMode = wifiMgr.isApMode();
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

    // Print WiFi info once, then start server
    {
      static bool wifiPrinted = false;
      if (!wifiPrinted && wifiIpPrinted) {
        wifiPrinted = true;
        if (!wifiMgr.isApMode()) {
          Serial.printf("\n  %-19s%s\n", "WiFi", "CONNECTED");
          Serial.printf("  %-19s%s\n", "SSID:", wifiMgr.getSSID());
          Serial.printf("  %-19s%s\n", "IP:", WiFi.localIP().toString().c_str());
          Serial.printf("  %-19shttp://%s/\n", "Dashboard:", WiFi.localIP().toString().c_str());
        } else {
          Serial.printf("\n  %-19s%s\n", "WiFi", "AP MODE");
          Serial.printf("  %-19s\"%s\" / \"%s\"\n", "SSID:", WiFi.softAPSSID().c_str(), WiFiManager::AP_PASS);
          Serial.printf("  %-19shttp://%s/\n", "Config:", WiFi.softAPIP().toString().c_str());
        }
      }
    }

    // Start webserver + Firebase after WiFi connects or AP starts
    {
      static bool serverStarted = false;
      if (!serverStarted && !wsServer.isRunning()) {
        if (wifiMgr.isConnected() || wifiMgr.isApMode()) {
          serverStarted = true;
          wsServer.startServer();
          Serial.printf("  %-19s%s\n", "WebSocket", "STARTED");
          if (wifiMgr.isConnected()) fbBridge.start();
          Serial.println();
          Serial.println("  Core 0: Network (WiFi, WebSocket, Firebase, OTA)");
          Serial.println("  Core 1: Sensor (ADC, Power, Limits)");
          Serial.println("  Type 'help' for commands");
          Serial.print("> ");
        }
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
  while (true) {
    fbBridge.loop();
    fbBridge.pushLatest();
    fbBridge.checkOtaTrigger();  // detects new firmware in RTDB /ota, triggers cloudOta
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
      int ch = powerCalc.getAutoZeroChannel();
      if (ch >= 0 && ch < NUM_CHANNELS) {
        const int batches = 32;
        float floors[batches];
        float origLPF[NUM_CHANNELS];
        for (int i = 0; i < NUM_CHANNELS; i++) {
          origLPF[i] = powerCalc.lpfAlpha[i];
          powerCalc.lpfAlpha[i] = 1.0f;
        }
        for (int b = 0; b < batches; b++) {
          floors[b] = powerCalc.runAutoZeroSingle(ch);
        }
        for (int i = 0; i < NUM_CHANNELS; i++) {
          powerCalc.lpfAlpha[i] = origLPF[i];
        }
        for (int i = 0; i < batches; i++) {
          for (int j = i + 1; j < batches; j++) {
            if (floors[j] < floors[i]) {
              float t = floors[i]; floors[i] = floors[j]; floors[j] = t;
            }
          }
        }
        float median = (batches % 2 == 1)
            ? floors[batches / 2]
            : (floors[batches / 2 - 1] + floors[batches / 2]) * 0.5f;
        powerCalc.setNoiseFloor(ch, median);
        nvs.saveNoiseFloor(ch, median);
        nvs.commit();
        Serial.printf("  [NVS] ch%d auto-zero complete (median of %d): %.3f A\n",
          ch + 1, batches, median);
      }
      powerCalc.cancelAutoZero();
    }

    powerCalc.update(deltaSeconds);

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      updateSharedData();

      // Persist energy to NVS every ~5s (60 cycles × 80ms)
      static uint32_t lastEnergySave = 0;
      if (millis() - lastEnergySave > 5000) {
        lastEnergySave = millis();
        for (int ch = 0; ch < NUM_CHANNELS; ch++) {
          nvs.saveEnergyKWh(ch, powerCalc.getEnergyKWh(ch));
        }
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

  Serial.println();
  Serial.println("  =============================================");
  Serial.println("   ESP32-S3 6-Channel Electricity Counter");
  Serial.println("  =============================================");
  Serial.printf("  CPU: %d MHz  |  Flash: %d MB  |  PSRAM: %s\n",
    getCpuFrequencyMhz(), ESP.getFlashChipSize() / (1024*1024),
    psramFound() ? "OK" : "N/A");
  Serial.println();

  Serial.printf("  %-19s%s\n", "NVS", "OK"); nvs.begin();

  // === Phase 2: Init hardware with defaults first ===
  Serial.printf("  %-19s%s\n", "Status LED", "OK"); statusLED.begin();
  Serial.printf("  %-19s%s\n", "Power Calculator", "OK"); powerCalc.begin();
  Serial.printf("  %-19s%s\n", "Buzzer", "OK"); buzzer.begin(PIN_BUZZER);
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

  Serial.printf("  %-19svoltage=%.1fV  current=%.1f (ch1)  RMS samples=%d\n",
    "Calibration", powerCalc.voltageCal, powerCalc.currentCal[0], powerCalc.rmsSamples);
  Serial.printf("  %-19sok\n", "Noise Floor + LPF");
  Serial.printf("  %s\n", "Channel Config");
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
    Serial.printf("    Ch%d  %-16s  %.1fkWh/mo\n",
      ch+1, name, systemData.channels[ch].monthlyKwhLimit);
  }

  dataMutex = xSemaphoreCreateMutex();
  Serial.printf("  %-19s%s\n", "Mutex", "OK");

  ntfyNotifier.begin(nvs.loadNtfyEnabled(), nvs.loadNtfyTopic());
  limitMgr.begin(nvs, powerCalc, &systemData, &dataMutex, &ntfyNotifier, &buzzer);

  // Shared text-command engine: drives both the serial console and the web
  // UI console (via processCommand -> WebSocket / Firebase).
  consoleHandler.begin(&nvs, &powerCalc, &systemData, &dataMutex, &buzzer,
                       &limitMgr, &wifiMgr, &otaHandler);

  WiFi.onEvent(onWiFiEvent);

  wsServer.begin(nvs, &systemData, &dataMutex, &powerCalc, &limitMgr);
  fbBridge.begin(nvs, &systemData, &dataMutex, &powerCalc, &limitMgr);
  cloudOta.begin();

  wifiMgr.begin(nvs);
  statusLED.setMode(LED_SOLID_RED);
  if (wifiMgr.isApMode()) {
    Serial.printf("  %-19sAP @ %s\n", "WiFi", WiFi.softAPIP().toString().c_str());
    Serial.printf("  %-19s\"%s\" / \"%s\"\n", "SSID", wifiMgr.getSSID(), WiFiManager::AP_PASS);
  } else {
    Serial.printf("  %-19s%s\n", "WiFi", "CONNECTING");
  }

  otaHandler.begin("esp32-elec-counter");
  Serial.printf("  %-19s%s\n", "OTA", "OK");

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
    Serial.println("  LED test: GREEN"); statusLED.setMode(LED_SOLID_GREEN); statusLED.loop(); delay(1500);
    Serial.println("  LED test: YELLOW (blink)"); statusLED.setMode(LED_BLINK_YELLOW); for (int i = 0; i < 6; i++) { statusLED.loop(); delay(400); }
    Serial.println("  LED test: RED"); statusLED.setMode(LED_SOLID_RED); statusLED.loop(); delay(1500);
    Serial.println("  LED test: RED (blink)"); statusLED.setMode(LED_BLINK_RED); for (int i = 0; i < 6; i++) { statusLED.loop(); delay(300); }
    Serial.println("  LED test: BLUE"); statusLED.setMode(LED_SOLID_BLUE); statusLED.loop(); delay(1500);
    statusLED.setMode(LED_SOLID_GREEN); statusLED.loop();
    Serial.println("  LED test done");
  }

  else if (cmd.startsWith("rms_samples ")) {
    int val = cmd.substring(12).toInt();
    if (val >= 100 && val <= MAX_RMS_SAMPLES) {
      powerCalc.setRmsSamples(val);
      nvs.saveRmsSamples(val);
      nvs.commit();
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        systemData.rmsSamples = val;
        xSemaphoreGive(dataMutex);
      }
      Serial.printf("  RMS samples set to %d\n", val);
    } else {
      Serial.printf("  RMS samples must be 100-%d\n", MAX_RMS_SAMPLES);
    }
  }

  else if (cmd.startsWith("curr_cal ")) {
    int sp = cmd.indexOf(' ', 9);
    if (sp > 0) {
      int ch = cmd.substring(9, sp).toInt() - 1;
      float val = cmd.substring(sp + 1).toFloat();
      if (ch >= 0 && ch < NUM_CHANNELS && val > 0) {
        powerCalc.currentCal[ch] = val;
        nvs.saveChannelCurrentCal(ch, val);
        nvs.commit();
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          systemData.currentCalibration[ch] = val;
          xSemaphoreGive(dataMutex);
        }
        Serial.printf("  Ch%d current calibration set to %.1f\n", ch + 1, val);
      }
    }
  }

  else if (cmd.startsWith("auto_zero ")) {
    int ch = cmd.substring(10).toInt() - 1;
    if (ch >= 0 && ch < NUM_CHANNELS) {
      powerCalc.requestAutoZero(ch);
      Serial.printf("  Ch%d auto-zero requested (runs on next sensor cycle)\n", ch + 1);
    }
  }

  else if (cmd.startsWith("volt_cal ")) {
    float val = cmd.substring(9).toFloat();
    if (val > 0) {
      powerCalc.voltageCal = val;
      nvs.saveVoltageCalibration(val);
      nvs.commit();
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        systemData.voltageCalibration = val;
        xSemaphoreGive(dataMutex);
      }
      Serial.printf("  Voltage calibration set to %.1f\n", val);
    }
  }

  else if (cmd == "setwifi sta") {
    nvs.saveWiFiMode(1);
    Serial.println("  WiFi mode: STA — will connect to saved SSID on next boot");
  }

  else if (cmd == "setwifi ap") {
    nvs.saveWiFiMode(2);
    Serial.println("  WiFi mode: AP — board will start as access point");
  }

  else if (cmd == "setwifi auto") {
    nvs.saveWiFiMode(0);
    Serial.println("  WiFi mode: AUTO — try STA first, fallback to AP");
  }

  else if (cmd.startsWith("setwifi ssid ")) {
    String ssid = cmd.substring(13);
    ssid.trim();
    if (ssid.length() > 0) {
      nvs.saveWiFiSSID(ssid);
      Serial.printf("  WiFi SSID saved: \"%s\"\n", ssid.c_str());
    } else {
      Serial.println("  Usage: setwifi ssid <network name>");
    }
  }

  else if (cmd.startsWith("setwifi pass ")) {
    String pass = cmd.substring(13);
    pass.trim();
    nvs.saveWiFiPass(pass);
    Serial.printf("  WiFi password saved (%d chars)\n", pass.length());
  }

  else if (cmd == "setwifi connect" || cmd == "setwifi save") {
    String ssid, pass;
    nvs.loadWiFi(ssid, pass);
    if (ssid.length() > 0) {
      nvs.saveWiFiMode(1);
      Serial.printf("  Connecting to \"%s\"... rebooting\n", ssid.c_str());
      delay(100);
      ESP.restart();
    } else {
      Serial.println("  No SSID set. Use 'setwifi ssid <name>' first");
    }
  }

  else if (cmd == "clearwifi") {
    nvs.clearWiFi();
    nvs.saveWiFiMode(0);
    Serial.println("  WiFi credentials + mode cleared");
    Serial.println("  Type 'reboot' to restart in AP mode");
  }

  else if (cmd == "nvs_debug") {
    Serial.println("  NVS Debug (cache read):");

    nvs.saveRmsSamples(888);
    uint16_t rr = nvs.loadRmsSamples();
    Serial.printf("    Write rms_samp=%d  Read(cache) rms_samp=%d  %s\n",
      888, rr, (rr == 888) ? "OK" : "FAIL");

    nvs.saveChannelCurrentCal(0, 7.5f);
    float rc = nvs.loadChannelCurrentCal(0);
    Serial.printf("    Write ch1_ccal=%.1f  Read(cache) ch1_ccal=%.1f  %s\n",
      7.5f, rc, (rc == 7.5f) ? "OK" : "FAIL");

    nvs.saveVoltageCalibration(42.5f);
    float rv = nvs.loadVoltageCalibration();
    Serial.printf("    Write volt_cal=%.1f  Read(cache) volt_cal=%.1f  %s\n",
      42.5f, rv, (rv == 42.5f) ? "OK" : "FAIL");

    // Now re-open handle and read from flash
    nvs.commit();
    rr = nvs.loadRmsSamples();
    rc = nvs.loadChannelCurrentCal(0);
    rv = nvs.loadVoltageCalibration();
    Serial.println("  NVS Debug (flash read after commit):");
    Serial.printf("    rms_samp=%d  ch1_ccal=%.1f  volt_cal=%.1f\n",
      rr, rc, rv);
  }

  else if (cmd == "reboot") {
    Serial.println("  Rebooting...");
    nvs.end();
    delay(1000);
    ESP.restart();
  }

  else {
    // Everything else (status, ch, cal, info, wifi, help, buzz, inject,
    // reset, reset_name) is handled by the shared ConsoleHandler so the serial
    // port and the web console stay in lockstep.
    String out;
    consoleHandler.exec(cmd, out);
    Serial.println();
    Serial.print(out);
    if (cmd == "help") {
      Serial.println("    test led            LED color sequence test");
      Serial.println("    rms_samples <N>     Set RMS samples (100-2000)");
      Serial.println("    curr_cal <ch> <val> Set current calibration for channel");
      Serial.println("    auto_zero <ch>      Auto-zero noise floor for channel");
      Serial.println("    volt_cal <val>      Set voltage calibration");
      Serial.println("    setwifi sta|ap|auto Set WiFi mode");
      Serial.println("    setwifi ssid <name> Set WiFi network name");
      Serial.println("    setwifi pass <pwd>  Set WiFi password");
      Serial.println("    setwifi connect     Save + reboot to connect");
      Serial.println("    clearwifi           Erase WiFi credentials");
      Serial.println("    nvs_debug           Test NVS write/read cycle");
      Serial.println("    reboot              Restart the device");
    }
    Serial.println();
  }
}

void loop() {
  // Run OTA download in the loopTask context (Core 1). This task is NOT
  // registered with the task watchdog, so long downloads don't trigger WDT
  // resets. The IDLE task on Core 0 is fed by the firebaseTask's regular
  // vTaskDelayUntil yields, so the WDT stays happy on both cores.
  cloudOta.loop();
  vTaskDelay(pdMS_TO_TICKS(100));
}

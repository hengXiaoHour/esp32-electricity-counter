#include "src/config.h"
#include "src/core/power_calculator.h"
#include "src/core/limit_manager.h"

#include "src/network/wifi_manager.h"
#include "src/network/websocket_server.h"
#include "src/network/ota_handler.h"
#include "src/network/ntfy_notifier.h"
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

    // Start webserver after WiFi connects or AP starts
    {
      static bool serverStarted = false;
      if (!serverStarted && !wsServer.isRunning()) {
        if (wifiMgr.isConnected() || wifiMgr.isApMode()) {
          serverStarted = true;
          wsServer.startServer();
          Serial.printf("  %-19s%s\n", "WebSocket", "STARTED");
          Serial.println();
          Serial.println("  Core 0: Network (WiFi, WebSocket, OTA)");
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

  WiFi.onEvent(onWiFiEvent);

  wsServer.begin(nvs, &systemData, &dataMutex, &powerCalc, &limitMgr);

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

  xTaskCreatePinnedToCore(networkTask, "network", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(sensorTask, "sensor", 8192, NULL, 2, NULL, 1);

  Serial.println();
}

// ==============================
// Serial Command Handler (testing)
// ==============================
static void handleSerialCommand(const String &cmd) {
  if (cmd == "status") {
    Serial.println();
    uint32_t up = millis() / 1000;
    Serial.printf("  %-16s%02lu:%02lu:%02lu\n", "Uptime:", up/3600, (up%3600)/60, up%60);
    Serial.printf("  %-16s%s  (RSSI: %d dBm)\n", "WiFi:",
      wifiMgr.isConnected() ? "CONNECTED" : wifiMgr.isApMode() ? "AP MODE" : "DISCONNECTED",
      wifiMgr.getRSSI());
    Serial.printf("  %-16s%.1f V\n", "Voltage:", powerCalc.getVoltageRMS());
    Serial.printf("  %-16s%s (%d%%)\n", "OTA:",
      otaHandler.isInProgress() ? "IN PROGRESS" : "IDLE",
      otaHandler.getProgress());
    Serial.println();
    Serial.println("  Channels");
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
      const char *s = systemData.channels[ch].status == STATUS_OK ? "OK" :
                       systemData.channels[ch].status == STATUS_WARNING ? "WARN" :
                       systemData.channels[ch].status == STATUS_TRIPPED ? "TRIP" : "OFF";
      Serial.printf("  Ch%d  %-16s %s  %5.2fA  %5.0fW  %5.0fVA  PF=%.3f  %6.3fkWh\n",
        ch + 1, systemData.channels[ch].name, s,
        systemData.channels[ch].currentRMS,
        systemData.channels[ch].activePower,
        systemData.channels[ch].apparentPower,
        systemData.channels[ch].powerFactor,
        systemData.channels[ch].energyKWh);
    }
    Serial.printf("  Events: %d\n", systemData.eventCount);
    Serial.println();
  }

  else if (cmd.startsWith("ch ")) {
    int ch = cmd.substring(3).toInt() - 1;
    if (ch >= 0 && ch < NUM_CHANNELS) {
      const char *s = systemData.channels[ch].status == STATUS_OK ? "OK" :
                       systemData.channels[ch].status == STATUS_WARNING ? "WARN" :
                       systemData.channels[ch].status == STATUS_TRIPPED ? "TRIP" : "OFF";
      Serial.println();
      Serial.printf("  Channel %d  %s\n", ch + 1, systemData.channels[ch].name);
      Serial.printf("  %-16s%.2f A\n", "Current:", powerCalc.getCurrentRMS(ch));
      Serial.printf("  %-16s%.1f W\n", "Active Power:", powerCalc.getActivePower(ch));
      Serial.printf("  %-16s%.1f VA\n", "Apparent:", powerCalc.getApparentPower(ch));
      Serial.printf("  %-16s%.3f\n", "Power Factor:", powerCalc.getPowerFactor(ch));
      Serial.printf("  %-16s%.3f kWh\n", "Energy:", powerCalc.getEnergyKWh(ch));
      Serial.printf("  %-16s%s\n", "Status:", s);
      Serial.printf("  %-16s%.1f\n", "Current Cal:", powerCalc.currentCal[ch]);
      Serial.printf("  %-16s%.3f A\n", "Noise Floor:", powerCalc.noiseFloor[ch]);
      Serial.println();
    }
  }

  else if (cmd == "test led") {
    Serial.println("  LED test: GREEN"); statusLED.setMode(LED_SOLID_GREEN); statusLED.loop(); delay(1500);
    Serial.println("  LED test: YELLOW (blink)"); statusLED.setMode(LED_BLINK_YELLOW); for (int i = 0; i < 6; i++) { statusLED.loop(); delay(400); }
    Serial.println("  LED test: RED"); statusLED.setMode(LED_SOLID_RED); statusLED.loop(); delay(1500);
    Serial.println("  LED test: RED (blink)"); statusLED.setMode(LED_BLINK_RED); for (int i = 0; i < 6; i++) { statusLED.loop(); delay(300); }
    Serial.println("  LED test: BLUE"); statusLED.setMode(LED_SOLID_BLUE); statusLED.loop(); delay(1500);
    statusLED.setMode(LED_SOLID_GREEN); statusLED.loop();
    Serial.println("  LED test done");
  }

  else if (cmd.startsWith("reset ")) {
    int ch = cmd.substring(6).toInt() - 1;
    if (ch >= 0 && ch < NUM_CHANNELS) {
      limitMgr.resetCounter((uint8_t)ch);
      Serial.printf("  Ch%d counter reset\n", ch + 1);
    }
  }

  else if (cmd.startsWith("buzz ")) {
    int n = cmd.substring(5).toInt();
    if (n >= 1 && n <= 6) {
      buzzer.ring((uint8_t)n);
      Serial.printf("  Buzzer ringing %d beeps\n", n);
    } else {
      Serial.println("  Usage: buzz <N> (1-6 beeps)");
    }
  }

  else if (cmd == "reset_name" || cmd.startsWith("reset_name ")) {
    int ch = -1;
    if (cmd.length() > 10) {
      ch = cmd.substring(11).toInt() - 1;
    }
    if (ch >= 0 && ch < NUM_CHANNELS) {
      nvs.clearChannelName(ch);
      strncpy(systemData.channels[ch].name, NVSManager::defaultChannelName(ch), MAX_CHANNEL_NAME_LEN - 1);
      systemData.channels[ch].name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
      Serial.printf("  Channel %d name reset to \"%s\"\n", ch + 1, systemData.channels[ch].name);
    } else {
      for (int i = 0; i < NUM_CHANNELS; i++) {
        nvs.clearChannelName(i);
        strncpy(systemData.channels[i].name, NVSManager::defaultChannelName(i), MAX_CHANNEL_NAME_LEN - 1);
        systemData.channels[i].name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
      }
      Serial.println("  All channel names reset to defaults");
    }
  }

  else if (cmd.startsWith("inject ")) {
    // inject <ch> <kwh> — set fake energy for testing
    int sp1 = cmd.indexOf(' ', 7);
    if (sp1 > 0) {
      int ch = cmd.substring(7, sp1).toInt() - 1;
      float kwh = cmd.substring(sp1 + 1).toFloat();
      if (ch >= 0 && ch < NUM_CHANNELS && kwh >= 0) {
        powerCalc.setEnergyKWh(ch, kwh);
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          systemData.channels[ch].energyKWh = kwh;
          xSemaphoreGive(dataMutex);
        }
        Serial.printf("  Ch%d energy injected: %.3f kWh\n", ch + 1, kwh);
      }
    }
  }

  else if (cmd == "cal") {
    Serial.println();
    Serial.printf("  %-20s%d\n", "RMS Samples:", powerCalc.rmsSamples);
    Serial.printf("  %-20s%.1f\n", "Voltage Cal:", powerCalc.voltageCal);
    Serial.println("  Current Calibration (A):");
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
      Serial.printf("    Ch%d: %.1f", ch + 1, powerCalc.currentCal[ch]);
      Serial.printf("  | Noise Floor: %.3f A", powerCalc.noiseFloor[ch]);
      Serial.println();
    }
    Serial.println();
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

  else if (cmd == "info") {
    Serial.println();
    Serial.printf("  %-16sESP32-S3 Electricity Counter v1.0\n", "Firmware:");
    Serial.printf("  %-16s%s %s\n", "Built:", __DATE__, __TIME__);
    Serial.printf("  %-16s4 MB Flash, 2 MB PSRAM\n", "Hardware:");
    Serial.printf("  %-16s240 MHz dual-core\n", "CPU:");
    Serial.printf("  %-16s%d\n", "Channels:", NUM_CHANNELS);
    Serial.printf("  %-16s%d-bit, %.1fV ref\n", "ADC:", ADC_RESOLUTION, ADC_REFERENCE_V);
    Serial.println();
  }

  else if (cmd == "help") {
    Serial.println();
    Serial.println("  Commands:");
    Serial.println("    help                Show available commands");
    Serial.println("    status              System status overview");
    Serial.println("    ch <N>              Channel details (1-6)");
    Serial.println("    test led            LED color sequence test");
    Serial.println("    reset <N>           Reset counter for channel (1-6)");
    Serial.println("    buzz <N>            Ring buzzer N beeps (1-6)");
    Serial.println("    setwifi sta|ap|auto Set WiFi mode");
    Serial.println("    setwifi ssid <name> Set WiFi network name");
    Serial.println("    setwifi pass <pwd>  Set WiFi password");
    Serial.println("    setwifi connect     Save + reboot to connect");
    Serial.println("    wifi                Show WiFi status");
    Serial.println("    clearwifi           Erase WiFi credentials");
    Serial.println("    reboot              Restart the device");
    Serial.println("    info                Firmware & hardware info");
    Serial.println("    cal                 Show calibration values");
    Serial.println("    rms_samples <N>     Set RMS samples (100-2000)");
    Serial.println("    curr_cal <ch> <val> Set current calibration for channel");
    Serial.println("    auto_zero <ch>      Auto-zero noise floor for channel");
    Serial.println("    volt_cal <val>      Set voltage calibration");
    Serial.println("    nvs_debug           Test NVS write/read cycle");
    Serial.println();
  }

  else if (cmd == "wifi") {
    String ssid, pass;
    nvs.loadWiFi(ssid, pass);
    uint8_t mode = nvs.loadWiFiMode();
    const char *modeStr[] = {"AUTO", "STA", "AP"};
    Serial.println();
    Serial.printf("  %-16s%s\n", "Mode:", mode <= 2 ? modeStr[mode] : "?");
    Serial.printf("  %-16s\"%s\"\n", "SSID:", ssid.c_str());
    Serial.printf("  %-16s%s\n", "Status:",
      wifiMgr.isConnected() ? "CONNECTED" : wifiMgr.isApMode() ? "AP MODE" : "DISCONNECTED");
    Serial.printf("  %-16s%d dBm\n", "RSSI:", wifiMgr.getRSSI());
    Serial.printf("  %-16s%s\n", "IP:", WiFi.localIP().toString().c_str());
    if (wifiMgr.isApMode())
      Serial.printf("  %-16s%s\n", "AP IP:", WiFi.softAPIP().toString().c_str());
    Serial.println();
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
    Serial.println("  Unknown. Type 'help' for commands");
  }
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(100));
}

#include "src/config.h"
#include "src/core/power_calculator.h"
#include "src/core/relay_controller.h"
#include "src/core/limit_manager.h"
#include "src/network/wifi_manager.h"
#include "src/network/websocket_server.h"
#include "src/network/ota_handler.h"
#include "src/ui/status_led.h"
#include "src/utils/nvs_manager.h"

NVSManager      nvs;
PowerCalculator powerCalc;
RelayController relays;
LimitManager    limitMgr;
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

  bool anyTripped = false;
  bool anyWarning = false;
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (systemData.channels[i].status == STATUS_TRIPPED) anyTripped = true;
    if (systemData.channels[i].status == STATUS_WARNING) anyWarning = true;
  }

  if (anyTripped)
    statusLED.setMode(LED_BLINK_RED);
  else if (anyWarning)
    statusLED.setMode(LED_BLINK_YELLOW);
  else
    statusLED.setMode(LED_SOLID_GREEN);

  statusLED.loop();
}

static void updateSharedData() {
  systemData.voltageRMS = powerCalc.getVoltageRMS();
  systemData.voltageCalibration = powerCalc.voltageCal;
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

    powerCalc.update(deltaSeconds);

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      updateSharedData();

      float currRMS[NUM_CHANNELS], actPower[NUM_CHANNELS];
      float pLimit[NUM_CHANNELS], cLimit[NUM_CHANNELS];
      for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        currRMS[ch] = powerCalc.getCurrentRMS(ch);
        actPower[ch] = powerCalc.getActivePower(ch);
        pLimit[ch] = systemData.channels[ch].powerLimit;
        cLimit[ch] = systemData.channels[ch].currentLimit;
      }

      limitMgr.check(currRMS, actPower, pLimit, cLimit,
                     systemData.channels, relays,
                     systemData.events, systemData.eventCount);

      xSemaphoreGive(dataMutex);
    }

    updateLED();
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SENSOR_CYCLE_INTERVAL_MS));
  }
}

// ==============================
// Arduino Entry Points
// ==============================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("  =============================================");
  Serial.println("   ESP32-S3 6-Channel Electricity Counter");
  Serial.println("  =============================================");
  Serial.printf("  CPU: %d MHz  |  Flash: %d MB  |  PSRAM: %s\n",
    getCpuFrequencyMhz(), ESP.getFlashChipSize() / (1024*1024),
    psramFound() ? "OK" : "N/A");
  Serial.println();

  Serial.printf("  %-19s%s\n", "NVS", "OK"); nvs.begin();
  Serial.printf("  %-19s%s\n", "Status LED", "OK"); statusLED.begin();
  Serial.printf("  %-19s%s\n", "Relays", "OK"); relays.begin();
  Serial.printf("  %-19s%s\n", "Limit Manager", "OK"); limitMgr.begin();
  Serial.printf("  %-19s%s\n", "Power Calculator", "OK"); powerCalc.begin();

  float vCal = nvs.loadVoltageCalibration();
  powerCalc.voltageCal = vCal;
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    powerCalc.currentCal[ch] = nvs.loadChannelCurrentCal(ch);
  }
  Serial.printf("  %-19svoltage=%.1fV  current=%.1f (ch1)\n", "Calibration", vCal, powerCalc.currentCal[0]);

  Serial.printf("  %s\n", "Channel Config");
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    char name[MAX_CHANNEL_NAME_LEN];
    float cLimit, pLimit;
    bool hasConfig = nvs.loadChannelConfig(ch, name, sizeof(name), cLimit, pLimit);
    if (!hasConfig) {
      strncpy(name, NVSManager::defaultChannelName(ch), MAX_CHANNEL_NAME_LEN - 1);
      name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
    }
    strncpy(systemData.channels[ch].name, name, MAX_CHANNEL_NAME_LEN - 1);
    systemData.channels[ch].name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
    systemData.channels[ch].currentLimit = cLimit;
    systemData.channels[ch].powerLimit = pLimit;
    systemData.channels[ch].status = STATUS_OK;
    systemData.channels[ch].relayOn = false;
    Serial.printf("    Ch%d  %-16s  %.1fA / %.0fW  %s\n",
      ch+1, name, cLimit, pLimit,
      ch < RELAY_CHANNEL_COUNT ? "RELAY" : "MONITOR");
  }

  for (int ch = 0; ch < RELAY_CHANNEL_COUNT; ch++) {
    systemData.channels[ch].relayOn = false;
  }

  dataMutex = xSemaphoreCreateMutex();
  Serial.printf("  %-19s%s\n", "Mutex", "OK");

  WiFi.onEvent(onWiFiEvent);

  wsServer.begin(nvs, relays, limitMgr, &systemData, &dataMutex, &powerCalc);
  Serial.printf("  %-19s%s\n", "LittleFS", "OK");

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
      Serial.printf("  Ch%d  %-16s %s  %5.2fA  %5.0fW  %5.0fVA  PF=%.3f  %6.3fkWh  %s\n",
        ch + 1, systemData.channels[ch].name, s,
        systemData.channels[ch].currentRMS,
        systemData.channels[ch].activePower,
        systemData.channels[ch].apparentPower,
        systemData.channels[ch].powerFactor,
        systemData.channels[ch].energyKWh,
        systemData.channels[ch].relayOn ? "RELAY ON" : "RELAY OFF");
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
      Serial.printf("  %-16s%s\n", "Relay:", systemData.channels[ch].relayOn ? "ON" : "OFF");
      Serial.printf("  %-16s%.1f A / %.0f W\n", "Limits:",
        systemData.channels[ch].currentLimit,
        systemData.channels[ch].powerLimit);
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

  else if (cmd.startsWith("test relay ")) {
    int r = cmd.substring(11).toInt() - 1;
    if (r >= 0 && r < NUM_RELAYS) {
      Serial.printf("  Relay %d (GPIO%d) test\n", r + 1, RELAY_PINS[r]);
      Serial.print("    ON ... "); relays.set(r, true); delay(1000); Serial.println("done");
      Serial.print("    OFF .. "); relays.set(r, false); delay(1000); Serial.println("done");
      Serial.print("    ON ... "); relays.set(r, true); Serial.println("done");
      Serial.println("  Relay test done");
    }
  }

  else if (cmd.startsWith("reset ")) {
    int ch = cmd.substring(6).toInt() - 1;
    if (ch >= 0 && ch < NUM_CHANNELS) {
      limitMgr.resetChannel(ch, relays, systemData.channels);
      Serial.printf("  Channel %d reset\n", ch + 1);
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

  else if (cmd == "info") {
    Serial.println();
    Serial.printf("  %-16sESP32-S3 Electricity Counter v1.0\n", "Firmware:");
    Serial.printf("  %-16s%s %s\n", "Built:", __DATE__, __TIME__);
    Serial.printf("  %-16s4 MB Flash, 2 MB PSRAM\n", "Hardware:");
    Serial.printf("  %-16s240 MHz dual-core\n", "CPU:");
    Serial.printf("  %-16s%d\n", "Channels:", NUM_CHANNELS);
    Serial.printf("  %-16s%d\n", "Relays:", NUM_RELAYS);
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
    Serial.println("    test relay <N>      Relay toggle test (1-4)");
    Serial.println("    reset <N>           Clear tripped channel");
    Serial.println("    setwifi sta|ap|auto Set WiFi mode");
    Serial.println("    setwifi ssid <name> Set WiFi network name");
    Serial.println("    setwifi pass <pwd>  Set WiFi password");
    Serial.println("    setwifi connect     Save + reboot to connect");
    Serial.println("    wifi                Show WiFi status");
    Serial.println("    clearwifi           Erase WiFi credentials");
    Serial.println("    reboot              Restart the device");
    Serial.println("    info                Firmware & hardware info");
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

  else if (cmd == "reboot") {
    Serial.println("  Rebooting...");
    delay(100);
    ESP.restart();
  }

  else {
    Serial.println("  Unknown. Type 'help' for commands");
  }
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(100));
}

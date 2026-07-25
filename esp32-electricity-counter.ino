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
  systemData.currentCalibration = powerCalc.currentCal[0];
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

    // Print IP when WiFi connects
    {
      static bool ipPrinted = false;
      if (wifiMgr.isConnected() && !ipPrinted) {
        ipPrinted = true;
        Serial.printf("\n*** WiFi CONNECTED ***\n");
        Serial.printf("  SSID: %s\n", wifiMgr.getSSID());
        Serial.printf("  IP:   %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("  DNS:  %s\n", WiFi.dnsIP().toString().c_str());
        Serial.printf("  Dashboard: http://%s/\n", WiFi.localIP().toString().c_str());
      }
      if (wifiMgr.isApMode() && !ipPrinted) {
        ipPrinted = true;
        Serial.printf("\n*** AP MODE ***\n");
        Serial.printf("  SSID: \"%s\" / \"%s\"\n", wifiMgr.getSSID(), WiFiManager::AP_PASS);
        Serial.printf("  IP:   %s\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("  Config page: http://%s/\n", WiFi.softAPIP().toString().c_str());
      }
    }

    // Start webserver when WiFi is connected (runs via tcpip_callback for LwIP safety)
    if (!wsServer.isRunning()) {
      if (wifiMgr.isConnected()) {
        Serial.println("[INIT] Starting WebSocket Server...");
        wsServer.startServer();
      } else if (wifiMgr.isApMode()) {
        // AP mode: WiFiManager's captive portal uses port 80, AsyncWebServer stays off
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
  Serial.println("\n\n========================================");
  Serial.println("ESP32-S3 6-Channel Electricity Counter");
  Serial.println("========================================");
  Serial.printf("CPU freq: %d MHz\n", getCpuFrequencyMhz());
  Serial.printf("Flash size: %d MB\n", ESP.getFlashChipSize() / (1024*1024));
  Serial.printf("PSRAM: %s\n", psramFound() ? "OK" : "N/A");

  Serial.print("[INIT] NVS... "); nvs.begin(); Serial.println("OK");
  Serial.print("[INIT] Status LED... "); statusLED.begin(); Serial.println("OK");
  Serial.print("[INIT] Relays... "); relays.begin(); Serial.println("OK");
  Serial.print("[INIT] Limit Manager... "); limitMgr.begin(); Serial.println("OK");
  Serial.print("[INIT] Power Calculator... "); powerCalc.begin(); Serial.println("OK");

  float vCal = nvs.loadVoltageCalibration();
  float cCal = nvs.loadCurrentCalibration();
  powerCalc.voltageCal = vCal;
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    powerCalc.currentCal[ch] = cCal;
  }
  Serial.printf("[INIT] Calibration: voltage=%.1f current=%.1f\n", vCal, cCal);

  Serial.println("[INIT] Loading channel configs...");
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
    systemData.channels[ch].relayOn = (ch < RELAY_CHANNEL_COUNT);
    Serial.printf("  Ch%d: \"%s\" %.1fA / %.0fW %s\n", ch+1, name, cLimit, pLimit,
      ch < RELAY_CHANNEL_COUNT ? "[RELAY]" : "[MONITOR]");
  }

  for (int ch = 0; ch < RELAY_CHANNEL_COUNT; ch++) {
    relays.reset(ch);
    systemData.channels[ch].relayOn = true;
  }
  Serial.println("[INIT] Relays initialized to ON (normally closed)");

  dataMutex = xSemaphoreCreateMutex();
  Serial.println("[INIT] Mutex created");

  Serial.print("[INIT] WiFi... ");
  wifiMgr.begin(nvs);
  statusLED.setMode(LED_SOLID_RED);
  if (wifiMgr.isApMode()) {
    Serial.printf("AP MODE @ %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("  Connect to SSID: \"%s\" / \"%s\"\n", wifiMgr.getSSID(), WiFiManager::AP_PASS);
  } else {
    Serial.println("CONNECTING (IP will be printed on connect)");
  }

  Serial.print("[INIT] WebSocket Server (deferred)... ");
  wsServer.begin(nvs, relays, limitMgr, &systemData, &dataMutex);
  Serial.println("LittleFS mounted (server starts after WiFi connects)");

  Serial.print("[INIT] OTA... ");
  otaHandler.begin("esp32-elec-counter");
  Serial.println("OK");

  xTaskCreatePinnedToCore(networkTask, "network", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(sensorTask, "sensor", 8192, NULL, 2, NULL, 1);

  Serial.println("\n✅ System running! Tasks active:");
  Serial.println("  Core 0: Network (WiFi, WebSocket, OTA)");
  Serial.println("  Core 1: Sensor (ADC, Power, Limits)");
  Serial.println("Commands: status, ch N, test led, test relay N, reset N,");
  Serial.println("  setwifi sta|ap|auto, setwifi ssid <n>, setwifi pass <p>, setwifi connect, wifi, clearwifi, reboot, info");
  Serial.print("> ");
}

// ==============================
// Serial Command Handler (testing)
// ==============================
static void handleSerialCommand(const String &cmd) {
  if (cmd == "status") {
    Serial.println("\n--- System Status ---");
    Serial.printf("Uptime: %lus\n", millis() / 1000);
    Serial.printf("WiFi: %s, RSSI: %d, AP: %s\n",
      wifiMgr.isConnected() ? "CONNECTED" : "DISCONNECTED",
      wifiMgr.getRSSI(), wifiMgr.isApMode() ? "YES" : "NO");
    Serial.printf("Voltage: %.1fV\n", powerCalc.getVoltageRMS());
    Serial.printf("OTA: %s (%d%%)\n",
      otaHandler.isInProgress() ? "IN PROGRESS" : "IDLE",
      otaHandler.getProgress());

    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
      const char *statusStr[] = {"OK", "WARN", "TRIP", "OFF"};
      Serial.printf("  Ch%d [%s]: %.2fA, %.0fW, %.0fVA, PF=%.3f, %.3fkWh, relay=%s\n",
        ch + 1, systemData.channels[ch].name,
        systemData.channels[ch].currentRMS,
        systemData.channels[ch].activePower,
        systemData.channels[ch].apparentPower,
        systemData.channels[ch].powerFactor,
        systemData.channels[ch].energyKWh,
        systemData.channels[ch].relayOn ? "ON" : "OFF");
    }
    Serial.printf("Events: %d\n", systemData.eventCount);
    Serial.println("---");
  }

  else if (cmd.startsWith("ch ")) {
    int ch = cmd.substring(3).toInt() - 1;
    if (ch >= 0 && ch < NUM_CHANNELS) {
      Serial.printf("Channel %d: %s\n", ch + 1, systemData.channels[ch].name);
      Serial.printf("  Current: %.2fA\n", powerCalc.getCurrentRMS(ch));
      Serial.printf("  Active Power: %.1fW\n", powerCalc.getActivePower(ch));
      Serial.printf("  Apparent: %.1fVA\n", powerCalc.getApparentPower(ch));
      Serial.printf("  PF: %.3f\n", powerCalc.getPowerFactor(ch));
      Serial.printf("  Energy: %.3fkWh\n", powerCalc.getEnergyKWh(ch));
      Serial.printf("  Status: %d, Relay: %s\n",
        systemData.channels[ch].status,
        systemData.channels[ch].relayOn ? "ON" : "OFF");
      Serial.printf("  Limits: %.1fA / %.0fW\n",
        systemData.channels[ch].currentLimit,
        systemData.channels[ch].powerLimit);
    }
  }

  else if (cmd == "test led") {
    Serial.println("Testing LED: GREEN");
    statusLED.setMode(LED_SOLID_GREEN); statusLED.loop(); delay(1000);
    Serial.println("Testing LED: YELLOW (blink)");
    statusLED.setMode(LED_BLINK_YELLOW); for (int i = 0; i < 4; i++) { statusLED.loop(); delay(500); }
    Serial.println("Testing LED: RED");
    statusLED.setMode(LED_SOLID_RED); statusLED.loop(); delay(1000);
    Serial.println("Testing LED: RED (blink)");
    statusLED.setMode(LED_BLINK_RED); for (int i = 0; i < 4; i++) { statusLED.loop(); delay(300); }
    Serial.println("Testing LED: BLUE");
    statusLED.setMode(LED_SOLID_BLUE); statusLED.loop(); delay(1000);
    statusLED.setMode(LED_SOLID_GREEN); statusLED.loop();
    Serial.println("LED test done");
  }

  else if (cmd.startsWith("test relay ")) {
    int r = cmd.substring(11).toInt() - 1;
    if (r >= 0 && r < NUM_RELAYS) {
      Serial.printf("Testing Relay %d (GPIO%d)\n", r + 1, RELAY_PINS[r]);
      Serial.println("  ON"); relays.set(r, true); delay(1000);
      Serial.println("  OFF"); relays.set(r, false); delay(1000);
      Serial.println("  ON"); relays.set(r, true);
      Serial.println("Relay test done");
    }
  }

  else if (cmd.startsWith("reset ")) {
    int ch = cmd.substring(6).toInt() - 1;
    if (ch >= 0 && ch < NUM_CHANNELS) {
      limitMgr.resetChannel(ch, relays, systemData.channels);
      Serial.printf("Channel %d reset\n", ch + 1);
    }
  }

  else if (cmd == "info") {
    Serial.printf("ESP32-S3 Electricity Counter v1.0\n");
    Serial.printf("Built: %s %s\n", __DATE__, __TIME__);
    Serial.printf("Flash: 4MB, PSRAM: 2MB\n");
    Serial.printf("CPU: 240MHz dual-core\n");
    Serial.printf("Channels: %d, Relays: %d\n", NUM_CHANNELS, NUM_RELAYS);
    Serial.printf("ADC: %d-bit, %.1fV ref\n", ADC_RESOLUTION, ADC_REFERENCE_V);
    Serial.printf("Help: status, ch N, test led, test relay N, reset N,\n");
    Serial.printf("  setwifi sta|ap|auto, setwifi ssid <name>, setwifi pass <pwd>,\n");
    Serial.printf("  setwifi connect|save, wifi, clearwifi, reboot, info\n");
  }

  else if (cmd == "wifi") {
    String ssid, pass;
    nvs.loadWiFi(ssid, pass);
    uint8_t mode = nvs.loadWiFiMode();
    const char *modeStr[] = {"AUTO", "STA", "AP"};
    Serial.printf("WiFi mode: %s\n", mode <= 2 ? modeStr[mode] : "?");
    Serial.printf("SSID: \"%s\" (password: %d chars)\n", ssid.c_str(), pass.length());
    Serial.printf("Status: %s\n", wifiMgr.isConnected() ? "CONNECTED" : wifiMgr.isApMode() ? "AP MODE" : "DISCONNECTED");
    Serial.printf("RSSI: %d dBm\n", wifiMgr.getRSSI());
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  }

  else if (cmd == "setwifi sta") {
    nvs.saveWiFiMode(1);
    Serial.println("WiFi mode: STA (station). Will connect to saved SSID on next boot.");
  }

  else if (cmd == "setwifi ap") {
    nvs.saveWiFiMode(2);
    Serial.println("WiFi mode: AP (access point). Board will always start as AP.");
  }

  else if (cmd == "setwifi auto") {
    nvs.saveWiFiMode(0);
    Serial.println("WiFi mode: AUTO. Will try STA first, fallback to AP.");
  }

  else if (cmd.startsWith("setwifi ssid ")) {
    String ssid = cmd.substring(13);
    ssid.trim();
    if (ssid.length() > 0) {
      nvs.saveWiFiSSID(ssid);
      Serial.printf("WiFi SSID saved: \"%s\"\n", ssid.c_str());
    } else {
      Serial.println("Usage: setwifi ssid <network name>");
    }
  }

  else if (cmd.startsWith("setwifi pass ")) {
    String pass = cmd.substring(13);
    pass.trim();
    nvs.saveWiFiPass(pass);
    Serial.printf("WiFi password saved (%d chars)\n", pass.length());
  }

  else if (cmd == "setwifi connect" || cmd == "setwifi save") {
    String ssid, pass;
    nvs.loadWiFi(ssid, pass);
    if (ssid.length() > 0) {
      nvs.saveWiFiMode(1);
      Serial.printf("Connecting to \"%s\"... Rebooting.\n", ssid.c_str());
      delay(100);
      ESP.restart();
    } else {
      Serial.println("No SSID set. Use 'setwifi ssid <name>' first.");
    }
  }

  else if (cmd == "clearwifi") {
    nvs.clearWiFi();
    nvs.saveWiFiMode(0);
    Serial.println("WiFi credentials + mode cleared from NVS.");
    Serial.println("Type 'reboot' to restart in AP mode.");
  }

  else if (cmd == "reboot") {
    Serial.println("Rebooting...");
    delay(100);
    ESP.restart();
  }

  else {
    Serial.println("Unknown. Try: status, ch N, test led, test relay N, reset N, setwifi ..., wifi, clearwifi, reboot, info");
  }
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(100));
}

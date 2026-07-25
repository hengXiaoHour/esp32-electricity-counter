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

  while (true) {
    wifiMgr.loop();
    wsServer.loop();
    otaHandler.loop();

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
  Serial.println(wifiMgr.isConnected() ? "CONNECTED" : "CONNECTING (AP fallback if no creds)");

  Serial.print("[INIT] WebSocket Server... ");
  wsServer.begin(nvs, relays, limitMgr, &systemData, &dataMutex);
  Serial.println("OK (serving from LittleFS)");

  Serial.print("[INIT] OTA... ");
  otaHandler.begin("esp32-elec-counter");
  Serial.println("OK");

  xTaskCreatePinnedToCore(networkTask, "network", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(sensorTask, "sensor", 8192, NULL, 2, NULL, 1);

  Serial.println("\n✅ System running! Tasks active:");
  Serial.println("  Core 0: Network (WiFi, WebSocket, OTA)");
  Serial.println("  Core 1: Sensor (ADC, Power, Limits)");
  Serial.println("Commands: status, ch N, test led, test relay N, reset N, info");
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
    Serial.printf("Help: status, ch N, test led, test relay N, reset N\n");
  }

  else {
    Serial.println("Unknown command. Try: status, ch N, test led, test relay N, reset N, info");
  }
}

void loop() {
  static char serialBuf[64];
  static uint8_t serialPos = 0;
  static uint32_t lastSerialStatus = 0;

  // Read serial commands
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialPos > 0) {
        serialBuf[serialPos] = '\0';
        handleSerialCommand(String(serialBuf));
        serialPos = 0;
      }
    } else if (serialPos < sizeof(serialBuf) - 1) {
      serialBuf[serialPos++] = c;
    }
  }

  // Periodic status every 30s
  if (millis() - lastSerialStatus > 30000) {
    lastSerialStatus = millis();
    Serial.printf("[%lus] WiFi:%s AP:%s V:%.1fV LED:", millis()/1000,
      wifiMgr.isConnected()?"Y":"N", wifiMgr.isApMode()?"Y":"N",
      powerCalc.getVoltageRMS());
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
      Serial.printf(" Ch%d:%.1fA", ch+1, powerCalc.getCurrentRMS(ch));
    }
    Serial.println();
  }

  vTaskDelay(pdMS_TO_TICKS(100));
}

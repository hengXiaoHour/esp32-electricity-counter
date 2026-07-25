#include "config.h"
#include "core/power_calculator.h"
#include "core/relay_controller.h"
#include "core/limit_manager.h"
#include "network/wifi_manager.h"
#include "network/websocket_server.h"
#include "network/ota_handler.h"
#include "ui/status_led.h"
#include "utils/nvs_manager.h"

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

static float getPowerLimit(int ch)   { return systemData.channels[ch].powerLimit; }
static float getCurrentLimit(int ch) { return systemData.channels[ch].currentLimit; }
static float getCurrentRMS(int ch)   { return powerCalc.getCurrentRMS(ch); }
static float getActivePower(int ch)  { return powerCalc.getActivePower(ch); }

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
        currRMS[ch] = getCurrentRMS(ch);
        actPower[ch] = getActivePower(ch);
        pLimit[ch] = getPowerLimit(ch);
        cLimit[ch] = getCurrentLimit(ch);
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
  Serial.println("\n\nESP32-S3 Electricity Counter Starting...");

  nvs.begin();
  statusLED.begin();
  relays.begin();
  limitMgr.begin();
  powerCalc.begin();

  powerCalc.voltageCal = nvs.loadVoltageCalibration();
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    powerCalc.currentCal[ch] = nvs.loadCurrentCalibration();
  }

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
  }

  for (int ch = 0; ch < RELAY_CHANNEL_COUNT; ch++) {
    relays.reset(ch);
    systemData.channels[ch].relayOn = true;
  }

  dataMutex = xSemaphoreCreateMutex();

  wifiMgr.begin(nvs);
  statusLED.setMode(LED_SOLID_RED);

  wsServer.begin(nvs, relays, limitMgr, &systemData, &dataMutex);
  otaHandler.begin("esp32-elec-counter");

  xTaskCreatePinnedToCore(networkTask, "network", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(sensorTask, "sensor", 8192, NULL, 2, NULL, 1);

  Serial.println("Tasks created. System running.");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

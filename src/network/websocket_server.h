#pragma once

#include <ESPAsyncWebServer.h>
#include "../config.h"
#include "../utils/nvs_manager.h"

#include "../core/power_calculator.h"

class LimitManager;

class WebSocketServer {
public:
  WebSocketServer();
  ~WebSocketServer();

  void begin(NVSManager &nvs,
             SystemData *sysData, SemaphoreHandle_t *mutex,
             PowerCalculator *powerCalc, LimitManager *limitMgr);
  void startServer();
  void stopServer();
  bool isRunning() const { return started; }
  void loop();

  void broadcastData(const SystemData &data);

private:
  AsyncWebServer *server;
  AsyncWebSocket *ws;
  NVSManager *nvs;

  PowerCalculator *powerCalc;
  SystemData *sysData;
  SemaphoreHandle_t *dataMutex;
  LimitManager *limitMgr;

  uint32_t lastBroadcast;
  bool started;

  void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                 AwsEventType type, void *arg, uint8_t *data, size_t len);

  void handleCommand(AsyncWebSocketClient *client, const char *msg);

  void buildJson(const SystemData &data, String &json);
};

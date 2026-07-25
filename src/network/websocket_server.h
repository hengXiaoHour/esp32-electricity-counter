#pragma once

#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "../config.h"
#include "../utils/nvs_manager.h"
#include "../core/relay_controller.h"
#include "../core/limit_manager.h"

class WebSocketServer {
public:
  void begin(NVSManager &nvs, RelayController &relays, LimitManager &limits);
  void loop();

  // Called from Core 1 sensor task (via shared data) to broadcast updates
  void broadcastData(const SystemData &data);

private:
  AsyncWebServer server;
  AsyncWebSocket ws;
  NVSManager *nvs;
  RelayController *relays;
  LimitManager *limits;

  uint32_t lastBroadcast;

  void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                 AwsEventType type, void *arg, uint8_t *data, size_t len);

  void handleCommand(AsyncWebSocketClient *client, const char *msg);

  // JSON serialization
  void buildJson(const SystemData &data, String &json);
  void buildChannelJson(const ChannelData &ch, String &json, bool last);
  void buildEventJson(const Event &ev, String &json, bool last);
};

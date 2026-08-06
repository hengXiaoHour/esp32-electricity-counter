#include "websocket_server.h"
#include "../core/limit_manager.h"
#include "command_processor.h"
#include "firebase_bridge.h"
#include <time.h>

WebSocketServer::WebSocketServer()
  : server(nullptr), ws(nullptr), lastBroadcast(0), started(false) {}

WebSocketServer::~WebSocketServer() {
  delete ws;
  delete server;
}

void WebSocketServer::begin(NVSManager &nvsRef,
                            SystemData *sysDataRef, SemaphoreHandle_t *mutexRef,
                            PowerCalculator *powerCalcRef, LimitManager *limitMgrRef) {
  nvs = &nvsRef;
  powerCalc = powerCalcRef;
  sysData = sysDataRef;
  dataMutex = mutexRef;
  limitMgr = limitMgrRef;
}

void WebSocketServer::startServer() {
  if (started) return;

  ws = new AsyncWebSocket("/ws");
  ws->onEvent([this](AsyncWebSocket *s, AsyncWebSocketClient *c,
                     AwsEventType t, void *a, uint8_t *d, size_t l) {
    onWsEvent(s, c, t, a, d, l);
  });

  server = new AsyncWebServer(80);
  server->addHandler(ws);

  server->onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  // server->begin() needs LwIP initialized + correct task context.
  // We call it directly here — it works after WiFi connects from networkTask.
  server->begin();
  started = true;
}

void WebSocketServer::stopServer() {
  if (!started) return;
  if (ws) { ws->closeAll(); delete ws; ws = nullptr; }
  if (server) { delete server; server = nullptr; }
  started = false;
}

void WebSocketServer::loop() {
  if (ws) ws->cleanupClients();
}

void WebSocketServer::broadcastData(const SystemData &data) {
  if (!started || !ws || ws->count() == 0) return;
  if (millis() - lastBroadcast < WS_UPDATE_INTERVAL_MS) return;
  lastBroadcast = millis();

  String json;
  buildJson(data, json);
  ws->textAll(json);
}

void WebSocketServer::onWsEvent(AsyncWebSocket *srv, AsyncWebSocketClient *client,
                                 AwsEventType type, void *arg,
                                 uint8_t *data, size_t len) {
  (void)srv;
  switch (type) {
    case WS_EVT_CONNECT:
    case WS_EVT_DISCONNECT:
      break;
    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (info->final && info->index == 0 && info->len == len &&
          info->opcode == WS_TEXT) {
        data[len] = 0;
        handleCommand(client, (const char *)data);
      }
      break;
    }
    default:
      break;
  }
}

void WebSocketServer::handleCommand(AsyncWebSocketClient *client, const char *msg) {
  (void)client;
  bool handled = processCommand(nvs, sysData, dataMutex, powerCalc, limitMgr, msg);
  if (handled) lastBroadcast = 0;  // force an immediate refresh
}

// --- JSON Serialization ---

void WebSocketServer::buildJson(const SystemData &data, String &json) {
  buildSystemJson(data, powerCalc, nvs, json);
}

#include "websocket_server.h"
#include "../core/limit_manager.h"
#include "command_processor.h"
#include "console_handler.h"
#include "firebase_bridge.h"
#include "wifi_manager.h"
#include <time.h>

WebSocketServer::WebSocketServer()
  : server(nullptr), ws(nullptr), lastBroadcast(0), started(false) {}

WebSocketServer::~WebSocketServer() {
  delete ws;
  delete server;
}

void WebSocketServer::begin(NVSManager &nvsRef,
                            SystemData *sysDataRef, SemaphoreHandle_t *mutexRef,
                            PowerCalculator *powerCalcRef, LimitManager *limitMgrRef,
                            WiFiManager *wifiMgrRef) {
  nvs = &nvsRef;
  powerCalc = powerCalcRef;
  sysData = sysDataRef;
  dataMutex = mutexRef;
  limitMgr = limitMgrRef;
  wifiMgr = wifiMgrRef;
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

  // Everything that is not /ws is an embedded dashboard asset. Registered as
  // the catch-all rather than one on() per file so adding an asset to
  // scripts/embed_web.py is all it takes to publish it.
  //
  // The old build served a WiFi-credentials form at "/" from ap_portal.h.
  // That form is gone: the board is AP-only, so there is no network to join,
  // and the captive-portal DNS wildcard already sends every URL to 192.168.4.1
  // where this dashboard is.
  server->onNotFound([](AsyncWebServerRequest *request) {
    if (WebSocketServer::serveAsset(request)) return;
    request->send(404, "text/plain", "Not found");
  });

  // server->begin() needs LwIP initialized + correct task context.
  // We call it directly here — it works after WiFi connects from networkTask.
  server->begin();
  started = true;
}

const WebAsset *WebSocketServer::findAsset(const String &path) {
  String want = path;
  int q = want.indexOf('?');
  if (q >= 0) want = want.substring(0, q);
  if (want.length() == 0 || want == "/") want = "/index.html";

  // Only exact matches. No prefix or directory walking: this is a fixed set of
  // 11 files, and "serve whatever the path walks to" is how a device turns
  // into an open file server.
  for (uint32_t i = 0; i < WEB_ASSET_COUNT; i++) {
    if (want == WEB_ASSETS[i].path) return &WEB_ASSETS[i];
  }
  return nullptr;
}

bool WebSocketServer::serveAsset(AsyncWebServerRequest *request) {
  const WebAsset *asset = findAsset(request->url());
  if (!asset) return false;

  // Explicit-length PROGMEM response. The uint8_t overload is required: the
  // PGM_P overload measures with strlen(), which would truncate every PNG at
  // its first 0x00 byte.
  AsyncWebServerResponse *res = request->beginResponse_P(
      200, asset->mime, (const uint8_t *)asset->data, asset->size);
  // The device is the only origin and the assets live in flash, so there is
  // nothing to revalidate against - force the browser to ask every time.
  res->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  request->send(res);
  return true;
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

size_t WebSocketServer::clientCount() const {
  return (ws) ? ws->count() : 0;
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
  String response;
  bool handled = processCommand(nvs, sysData, dataMutex, powerCalc, limitMgr,
                                msg, &response);
  // Console commands answer with text — ship it back to the requesting client
  // only (broadcasting it to every client would spam other dashboards).
  if (client && response.length() > 0) {
    String out = "{\"type\":\"console\",\"out\":\"";
    out += consoleJsonEscape(response);
    out += "\"}";
    client->text(out);
  }
  if (handled) lastBroadcast = 0;  // force an immediate refresh
}

// --- JSON Serialization ---

void WebSocketServer::buildJson(const SystemData &data, String &json) {
  buildSystemJson(data, powerCalc, nvs, json);
}

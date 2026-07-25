#include "websocket_server.h"
#include <LittleFS.h>

WebSocketServer::WebSocketServer()
  : server(nullptr), ws(nullptr), lastBroadcast(0), started(false) {}

WebSocketServer::~WebSocketServer() {
  delete ws;
  delete server;
}

void WebSocketServer::begin(NVSManager &nvsRef, RelayController &relaysRef,
                            LimitManager &limitsRef,
                            SystemData *sysDataRef, SemaphoreHandle_t *mutexRef,
                            PowerCalculator *powerCalcRef) {
  nvs = &nvsRef;
  relays = &relaysRef;
  limits = &limitsRef;
  powerCalc = powerCalcRef;
  sysData = sysDataRef;
  dataMutex = mutexRef;

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
  String s(msg);

  if (s.indexOf("\"cmd\":\"set_limit\"") >= 0) {
    int ch = -1; float val = 0;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) val = s.substring(vi + 6).toFloat();
    if (ch >= 0 && ch < NUM_CHANNELS && val > 0) {
      nvs->saveChannelConfig(ch, "", val, 0);
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->channels[ch].currentLimit = val;
        xSemaphoreGive(*dataMutex);
      }
    }

  } else if (s.indexOf("\"cmd\":\"set_power_limit\"") >= 0) {
    int ch = -1; float val = 0;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) val = s.substring(vi + 6).toFloat();
    if (ch >= 0 && ch < NUM_CHANNELS && val > 0) {
      nvs->saveChannelConfig(ch, "", 0, val);
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->channels[ch].powerLimit = val;
        xSemaphoreGive(*dataMutex);
      }
    }

  } else if (s.indexOf("\"cmd\":\"set_name\"") >= 0) {
    int ch = -1;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int ni = s.indexOf("\"name\":\"");
    if (ni >= 0 && ch >= 0 && ch < NUM_CHANNELS) {
      ni += 8;
      int end = s.indexOf("\"", ni);
      if (end > ni) {
        String name = s.substring(ni, end);
        if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          float clim = sysData->channels[ch].currentLimit;
          float plim = sysData->channels[ch].powerLimit;
          nvs->saveChannelConfig(ch, name.c_str(), clim, plim);
          strncpy(sysData->channels[ch].name, name.c_str(), MAX_CHANNEL_NAME_LEN - 1);
          sysData->channels[ch].name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
          xSemaphoreGive(*dataMutex);
        }
      }
    }

  } else if (s.indexOf("\"cmd\":\"reset_relay\"") >= 0) {
    int ch = -1;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    if (ch >= 0 && ch < NUM_CHANNELS && sysData && dataMutex && nvs && relays) {
      char name[MAX_CHANNEL_NAME_LEN];
      float oldClim, oldPlim;
      nvs->loadChannelConfig(ch, name, sizeof(name), oldClim, oldPlim);
      nvs->saveChannelConfig(ch, name, DEFAULT_CURRENT_LIMIT_A, DEFAULT_POWER_LIMIT_W);
      relays->set(ch, false);
      if (xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->channels[ch].currentLimit = DEFAULT_CURRENT_LIMIT_A;
        sysData->channels[ch].powerLimit = DEFAULT_POWER_LIMIT_W;
        sysData->channels[ch].relayOn = false;
        if (sysData->channels[ch].status == STATUS_TRIPPED) {
          sysData->channels[ch].status = STATUS_OK;
        }
        sysData->channels[ch].energyKWh = 0.0f;
        xSemaphoreGive(*dataMutex);
      }
      if (powerCalc) {
        powerCalc->resetEnergy(ch);
      }
      lastBroadcast = 0;
      printf("[WS] reset_relay: ch=%d limits=%.0fA/%.0fW relay=OFF energy=0\n",
             ch, DEFAULT_CURRENT_LIMIT_A, DEFAULT_POWER_LIMIT_W);
    }

  } else if (s.indexOf("\"cmd\":\"test_inject\"") >= 0) {
    int ch = -1; float val = 0;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) val = s.substring(vi + 6).toFloat();
    if (ch >= 0 && ch < NUM_CHANNELS && val >= 0 && powerCalc) {
      powerCalc->setEnergyKWh(ch, val);
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->channels[ch].energyKWh = val;
        xSemaphoreGive(*dataMutex);
      }
      lastBroadcast = 0;
      printf("[WS] test_inject: ch=%d energy=%.3f kWh\n", ch, val);
    }

  } else if (s.indexOf("\"cmd\":\"set_relay\"") >= 0) {
    int ch = -1;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int si = s.indexOf("\"state\":");
    if (ch >= 0 && ch < NUM_CHANNELS && si >= 0 && relays) {
      bool state = s.substring(si + 8, si + 12) == "true";
      printf("[WS] set_relay: ch=%d state=%s\n", ch, state ? "ON" : "OFF");
      relays->set(ch, state);
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->channels[ch].relayOn = state;
        xSemaphoreGive(*dataMutex);
      }
    } else {
      printf("[WS] set_relay: PARSE ERROR — raw: %s\n", msg);
    }

  } else if (s.indexOf("\"cmd\":\"set_voltage_cal\"") >= 0) {
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) {
      float val = s.substring(vi + 6).toFloat();
      nvs->saveVoltageCalibration(val);
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->voltageCalibration = val;
        xSemaphoreGive(*dataMutex);
      }
    }

  } else if (s.indexOf("\"cmd\":\"get_logs\"") >= 0) {
    int di = s.indexOf("\"date\":\"");
    if (di >= 0) {
      di += 8;
      int end = s.indexOf("\"", di);
      String date = s.substring(di, end);
      String path = String(LOG_DIR) + "/" + date;
      if (!path.endsWith(".csv")) path += ".csv";
      if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        String csv;
        while (f.available()) csv += (char)f.read();
        f.close();
        csv.replace("\\", "\\\\");
        csv.replace("\"", "\\\"");
        csv.replace("\n", "\\n");
        String resp = "{\"cmd\":\"log_data\",\"date\":\"";
        resp += date;
        resp += "\",\"csv\":\"";
        resp += csv;
        resp += "\"}";
        client->text(resp);
      } else {
        String resp = "{\"cmd\":\"log_data\",\"date\":\"";
        resp += date;
        resp += "\",\"csv\":\"\"}";
        client->text(resp);
      }
    } else {
      File root = LittleFS.open(LOG_DIR);
      String files = "[";
      bool first = true;
      if (root) {
        File f;
        while ((f = root.openNextFile())) {
          if (!first) files += ",";
          String fn = f.name();
          fn.replace(".csv", "");
          files += "\"" + fn + "\"";
          first = false;
          f.close();
        }
        root.close();
      }
      files += "]";
      String resp = "{\"cmd\":\"log_list\",\"files\":" + files + "}";
      client->text(resp);
    }

  } else if (s.indexOf("\"cmd\":\"set_current_cal\"") >= 0) {
    int ch = -1; float val = 0;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) val = s.substring(vi + 6).toFloat();
    if (ch >= 0 && ch < NUM_CHANNELS && val > 0) {
      nvs->saveChannelCurrentCal(ch, val);
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->currentCalibration[ch] = val;
        xSemaphoreGive(*dataMutex);
      }
    }

  } else if (s.indexOf("\"cmd\":\"set_monthly_kwh\"") >= 0) {
    int ch = -1; float val = 0;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) val = s.substring(vi + 6).toFloat();
    if (ch >= 0 && ch < NUM_CHANNELS && val > 0) {
      nvs->saveMonthlyKwhLimit(ch, val);
      if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysData->channels[ch].monthlyKwhLimit = val;
        xSemaphoreGive(*dataMutex);
      }
    }

  } else if (s.indexOf("\"cmd\":\"set_noise_floor\"") >= 0) {
    int ch = -1;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    if (ch >= 0 && ch < NUM_CHANNELS) {
      int vi = s.indexOf("\"val\":");
      if (vi >= 0) {
        float val = s.substring(vi + 6).toFloat();
        powerCalc->setNoiseFloor(ch, val);
        nvs->saveNoiseFloor(ch, val);
        printf("[WS] set_noise_floor: ch=%d val=%.3f\n", ch, val);
      } else {
        powerCalc->requestAutoZero(ch);
        printf("[WS] set_noise_floor: ch=%d auto-zero requested\n", ch);
      }
    }

  } else if (s.indexOf("\"cmd\":\"set_lpf_alpha\"") >= 0) {
    int ch = -1; float val = 1.0f;
    int ci = s.indexOf("\"ch\":");
    if (ci >= 0) ch = s.substring(ci + 5).toInt();
    int vi = s.indexOf("\"val\":");
    if (vi >= 0) val = s.substring(vi + 6).toFloat();
    if (ch >= 0 && ch < NUM_CHANNELS && val >= 0.01f && val <= 1.0f) {
      powerCalc->setLpfAlpha(ch, val);
      nvs->saveLpfAlpha(ch, val);
      printf("[WS] set_lpf_alpha: ch=%d val=%.2f\n", ch, val);
    }

  } else if (s.indexOf("\"cmd\":\"reset_channel_names\"") >= 0) {
    for (int i = 0; i < NUM_CHANNELS; i++) {
      nvs->clearChannelConfig(i);
    }
    if (sysData && dataMutex && xSemaphoreTake(*dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      for (int i = 0; i < NUM_CHANNELS; i++) {
        strncpy(sysData->channels[i].name, NVSManager::defaultChannelName(i), MAX_CHANNEL_NAME_LEN - 1);
        sysData->channels[i].name[MAX_CHANNEL_NAME_LEN - 1] = '\0';
      }
      xSemaphoreGive(*dataMutex);
    }
  }
}

// --- JSON Serialization ---

void WebSocketServer::buildJson(const SystemData &data, String &json) {
  json = "{\"v\":";
  json += String(data.voltageRMS, 1);
  json += ",\"uptime\":";
  json += data.uptime;
  json += ",\"wifi\":";
  json += data.wifiConnected ? "true" : "false";
  json += ",\"rssi\":";
  json += data.wifiRSSI;
  json += ",\"ap\":";
  json += data.apMode ? "true" : "false";
  json += ",\"ota\":";
  json += data.otaInProgress ? "true" : "false";
  json += ",\"otaProgress\":";
  json += data.otaProgress;
  json += ",\"voltageCalibration\":";
  json += String(data.voltageCalibration, 1);
  json += ",\"currentCalibration\":[";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    json += String(data.currentCalibration[i], 1);
    if (i < NUM_CHANNELS - 1) json += ",";
  }
  json += "]";
  json += ",\"noiseFloor\":[";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    json += String(powerCalc->noiseFloor[i], 3);
    if (i < NUM_CHANNELS - 1) json += ",";
  }
  json += "]";
  json += ",\"lpfAlpha\":[";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    json += String(powerCalc->lpfAlpha[i], 2);
    if (i < NUM_CHANNELS - 1) json += ",";
  }
  json += "]";
  json += ",\"firmwareVersion\":\"";
  json += FIRMWARE_VERSION;
  json += "\",\"ch\":[";

  for (int i = 0; i < NUM_CHANNELS; i++) {
    buildChannelJson(data.channels[i], i, json, i == NUM_CHANNELS - 1);
  }

  json += "],\"events\":[";

  int start = data.eventCount > 10 ? data.eventCount - 10 : 0;
  for (int i = start; i < data.eventCount; i++) {
    buildEventJson(data.events[i], json, i == data.eventCount - 1);
  }

  json += "]}";
}

void WebSocketServer::buildChannelJson(const ChannelData &ch, int index, String &json, bool last) {
  json += "{\"n\":\"";
  json += ch.name;
  json += "\",\"a\":";
  json += String(ch.currentRMS, 2);
  json += ",\"w\":";
  json += String(ch.activePower, 1);
  json += ",\"va\":";
  json += String(ch.apparentPower, 1);
  json += ",\"pf\":";
  json += String(ch.powerFactor, 3);
  json += ",\"kwh\":";
  json += String(ch.energyKWh, 3);
  json += ",\"s\":";
  json += ch.status;
  json += ",\"r\":";
  json += ch.relayOn ? "true" : "false";
  json += ",\"hasRelay\":";
  json += (index < RELAY_CHANNEL_COUNT) ? "true" : "false";
  json += ",\"cl\":";
  json += String(ch.currentLimit, 1);
  json += ",\"pl\":";
  json += String(ch.powerLimit, 0);
  json += ",\"mkwh\":";
  json += String(ch.monthlyKwhLimit, 1);
  json += "}";
  if (!last) json += ",";
}

void WebSocketServer::buildEventJson(const Event &ev, String &json, bool last) {
  json += "{\"t\":";
  json += ev.timestamp;
  json += ",\"c\":";
  json += ev.channel;
  json += ",\"s\":";
  json += ev.status;
  json += ",\"v\":";
  json += String(ev.value, 1);
  json += ",\"m\":\"";
  json += ev.message;
  json += "\"}";
  if (!last) json += ",";
}

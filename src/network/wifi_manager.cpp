#include "wifi_manager.h"

const char *WiFiManager::AP_SSID = AP_SSID_DEFAULT;
const char *WiFiManager::AP_PASS = AP_PASS_DEFAULT;

void WiFiManager::begin(NVSManager &nvsRef) {
  state = WIFI_INIT;
  nvs = &nvsRef;
  retryCount = 0;
  lastRetry = 0;
  rssi = 0;
  httpServer = nullptr;

  // Load WiFi credentials
  nvs->loadWiFi(configuredSSID, configuredPass);
  uint8_t mode = nvs->loadWiFiMode();

  if (mode == 2) {
    // AP-only mode
    startAPMode();
  } else if (configuredSSID.length() > 0) {
    // STA mode (or AUTO with credentials)
    state = WIFI_CONNECTING;
    connectToWiFi();
  } else {
    startAPMode();
  }
}

static uint8_t flapCount = 0;

void WiFiManager::loop() {
  if (state == WIFI_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      state = WIFI_CONNECTED;
      flapCount = 0;
      rssi = WiFi.RSSI();
      if (httpServer) {
        stopAPMode();
      }
    } else if (millis() - lastRetry >= WIFI_RETRY_INTERVAL_MS) {
      retryCount++;
      if (retryCount >= WIFI_MAX_RETRIES) {
        startAPMode();
      } else {
        connectToWiFi();
      }
    }
  }

  if (state == WIFI_CONNECTED) {
    // Update RSSI periodically
    checkConnection();
    if (state != WIFI_CONNECTED) {
      retryCount = 0;
      connectToWiFi();
    } else {
      // Update RSSI every 5s
      static uint32_t lastRSSI = 0;
      if (millis() - lastRSSI > 5000) {
        rssi = WiFi.RSSI();
        lastRSSI = millis();
      }
    }
  }

  if (state == WIFI_AP_MODE) {
    dnsServer.processNextRequest();
    if (httpServer) {
      httpServer->handleClient();
    }
  }
}

const char *WiFiManager::getSSID() const {
  if (state == WIFI_CONNECTED) {
    return configuredSSID.c_str();
  }
  return AP_SSID;
}

void WiFiManager::checkConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    flapCount = 0;
  } else {
    flapCount++;
    if (flapCount >= 3) {
      state = WIFI_CONNECTING;
    }
  }
}

void WiFiManager::connectToWiFi() {
  if (configuredSSID.length() == 0) {
    startAPMode();
    return;
  }
  lastRetry = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(configuredSSID.c_str(), configuredPass.c_str());
}

void WiFiManager::startAPMode() {
  state = WIFI_AP_MODE;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  // Start DNS server to capture all requests
  dnsServer.start(53, "*", apIP);

  // Start HTTP server for config page
  if (!httpServer) {
    httpServer = new WebServer(80);
  }

  httpServer->on("/", [this]() { handleAPRoot(); });
  httpServer->on("/save", [this]() { handleAPSave(); });
  httpServer->onNotFound([this]() { handleAPNotFound(); });
  httpServer->begin();
}

void WiFiManager::stopAPMode() {
  if (httpServer) {
    httpServer->stop();
    delete httpServer;
    httpServer = nullptr;
  }
  dnsServer.stop();
}

void WiFiManager::handleAPRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 WiFi Setup</title>
<style>
body{font-family:sans-serif;margin:20px;max-width:400px}
input{width:100%;padding:8px;margin:6px 0;box-sizing:border-box}
button{width:100%;padding:10px;background:#2196F3;color:#fff;border:none;border-radius:4px;font-size:16px}
</style>
</head>
<body>
<h2>ESP32 Electricity Counter</h2>
<p>Configure WiFi</p>
<form action="/save" method="POST">
<label>SSID</label>
<input type="text" name="ssid" required>
<label>Password</label>
<input type="password" name="pass">
<button type="submit">Save & Connect</button>
</form>
</body>
</html>
)rawliteral";
  httpServer->send(200, "text/html", html);
}

void WiFiManager::handleAPSave() {
  String ssid = httpServer->arg("ssid");
  String pass = httpServer->arg("pass");

  if (ssid.length() == 0) {
    httpServer->send(400, "text/plain", "SSID required");
    return;
  }

  nvs->saveWiFi(ssid, pass);
  configuredSSID = ssid;
  configuredPass = pass;

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title>
<style>body{font-family:sans-serif;margin:20px;text-align:center;padding-top:40px}</style>
</head>
<body>
<h2>Saved!</h2>
<p>Connecting to )rawliteral" + ssid + R"rawliteral(...</p>
<p>The device will restart. Reconnect to your WiFi and open the dashboard.</p>
</body>
</html>
)rawliteral";

  httpServer->send(200, "text/html", html);

  delay(100);
  state = WIFI_CONNECTING;
  retryCount = 0;
  stopAPMode();
  connectToWiFi();
}

void WiFiManager::handleAPNotFound() {
  httpServer->send(304, "text/plain", "");
}

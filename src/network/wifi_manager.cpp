#include "wifi_manager.h"

const char *WiFiManager::AP_SSID = AP_SSID_DEFAULT;
const char *WiFiManager::AP_PASS = AP_PASS_DEFAULT;

void WiFiManager::begin(NVSManager &nvsRef) {
  state = WIFI_INIT;
  nvs = &nvsRef;
  retryCount = 0;
  lastRetry = 0;
  rssi = 0;

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

// Consecutive failed-boot counter. Lives in RTC memory so it survives
// ESP.restart() but resets on power-cycle. Caps the reboot-loop failsafe.
RTC_DATA_ATTR uint8_t wifiBootFailures = 0;

static uint8_t flapCount = 0;

void WiFiManager::loop() {
  if (state == WIFI_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      state = WIFI_CONNECTED;
      flapCount = 0;
      wifiBootFailures = 0;
      rssi = WiFi.RSSI();
      // DNS only runs while in AP mode — shut it down on the way to STA.
      // DNSServer::stop() on a never-started server is a safe no-op.
      stopAPMode();
    } else if (millis() - connectStart >= WIFI_CONNECT_TIMEOUT_MS) {
      handleConnectTimeout();
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
  connectStart = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(configuredSSID.c_str(), configuredPass.c_str());
  configTime(0, 0, "pool.ntp.org", "time.google.com");
}

// Failsafe: if no link after WIFI_CONNECT_TIMEOUT_MS, reboot automatically.
// Cap the reboot loop at WIFI_MAX_BOOT_FAILURES consecutive failures so a
// device with wrong/stale credentials still lands in AP mode for reconfig.
void WiFiManager::handleConnectTimeout() {
  wifiBootFailures++;
  Serial.printf("[WiFi] No connection after %d ms (attempt %u) — rebooting...\n",
                WIFI_CONNECT_TIMEOUT_MS, wifiBootFailures);
  if (wifiBootFailures >= WIFI_MAX_BOOT_FAILURES) {
    wifiBootFailures = 0;
    startAPMode();
    return;
  }
  ESP.restart();
}

void WiFiManager::startAPMode() {
  state = WIFI_AP_MODE;
  wifiBootFailures = 0;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  // DNS captive-portal redirect stays here (port 53, no conflict).
  // HTTP portal pages now live on the shared AsyncWebServer:80
  // (WebSocketServer::startServer registers them in AP mode).
  dnsServer.start(53, "*", apIP);
}

void WiFiManager::stopAPMode() {
  dnsServer.stop();
}

bool WiFiManager::saveCredentialsAndConnect(const String &ssid, const String &pass) {
  String s = ssid;
  s.trim();
  if (s.length() == 0) return false;

  nvs->saveWiFi(s, pass);
  configuredSSID = s;
  configuredPass = pass;

  state = WIFI_CONNECTING;
  retryCount = 0;
  stopAPMode();
  connectToWiFi();
  return true;
}

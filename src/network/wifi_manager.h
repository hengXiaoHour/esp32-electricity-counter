#pragma once

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include "../config.h"
#include "../utils/nvs_manager.h"

enum WifiState : uint8_t {
  WIFI_INIT = 0,
  WIFI_CONNECTING,
  WIFI_CONNECTED,
  WIFI_AP_MODE,
  WIFI_FAILED
};

class WiFiManager {
public:
  void begin(NVSManager &nvs);
  void loop();

  WifiState getState() const        { return state; }
  bool isConnected() const          { return state == WIFI_CONNECTED; }
  bool isApMode() const             { return state == WIFI_AP_MODE; }
  int8_t getRSSI() const            { return rssi; }
  const char *getSSID() const;

  // AP credentials for fallback mode
  static const char *AP_SSID;
  static const char *AP_PASS;

private:
  WifiState state;
  NVSManager *nvs;
  String configuredSSID;
  String configuredPass;
  int retryCount;
  uint32_t lastRetry;
  uint32_t connectStart;
  int8_t rssi;

  DNSServer dnsServer;
  WebServer *httpServer;

  void connectToWiFi();
  void startAPMode();
  void stopAPMode();
  void checkConnection();
  void handleConnectTimeout();

  // AP config web page handlers
  void handleAPRoot();
  void handleAPSave();
  void handleAPNotFound();
};

// AP credentials
constexpr const char *AP_SSID_DEFAULT = "ESP32-Elec-Counter";
constexpr const char *AP_PASS_DEFAULT = "configure123";

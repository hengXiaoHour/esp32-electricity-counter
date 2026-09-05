#pragma once

#include <WiFi.h>
#include <DNSServer.h>
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

  // Eco mode: enable/disable modem sleep to save power when nobody is
  // watching. Only acts while STA-connected (an AP must keep beaconing).
  // Redundant calls are cheap no-ops.
  void setEcoSleep(bool eco);

  // Store STA credentials from the AP portal (served by WebSocketServer on
  // the shared AsyncWebServer) and switch to STA connect. Returns false
  // when the SSID is empty.
  bool saveCredentialsAndConnect(const String &ssid, const String &pass);

  // Runs before ESP.restart() in the STA connect-timeout path so counter
  // data survives the reboot. Set by the sketch (WiFiManager can't see the
  // power calculator); may be null.
  void setPreRestartFlush(void (*cb)()) { preRestartFlush = cb; }

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

  void (*preRestartFlush)() = nullptr;
  bool ecoSleep = false;

  void connectToWiFi();
  void startAPMode();
  void stopAPMode();
  void checkConnection();
  void handleConnectTimeout();
};

// AP credentials
constexpr const char *AP_SSID_DEFAULT = "ESP32-Elec-Counter";
constexpr const char *AP_PASS_DEFAULT = "configure123";

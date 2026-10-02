#pragma once

#include <WiFi.h>
#include <DNSServer.h>
#include "../config.h"

// The board is AP-only. There is no station interface, no upstream network,
// and no cloud: the ESP32 publishes a WiFi network of its own, serves the
// whole dashboard from it, and the phone's browser talks to
// ws://192.168.4.1/ws.
//
// That single decision removed, from this class: the STA connect path, the
// retry ladder, the connect-timeout reboot failsafe, the RTC boot-failure
// counter, link-flap detection, RSSI sampling, and modem-sleep eco mode. An AP
// must keep beaconing, so there was never an eco case to preserve.
//
// WiFi credentials may still sit in NVS from an older firmware. They are
// deliberately left untouched (see `clearwifi`) so downgrading to an older
// build still finds them - but nothing reads them any more.
enum WifiState : uint8_t {
  WIFI_INIT = 0,
  WIFI_AP_MODE,
};

class WiFiManager {
public:
  void begin();
  void loop();

  // True once softAP() has returned and the dashboard server may be started.
  // Replaces the old `isConnected() || isApMode()` pair: there is only one
  // state, so a two-way test would be a lie waiting to happen.
  bool isReady() const { return state == WIFI_AP_MODE; }

  const char *getSSID() const { return AP_SSID; }

  // Phones/laptops currently associated with the AP. The AP-mode analogue of
  // the old STA RSSI + WS client count, and the only "is anyone watching"
  // signal that still exists.
  uint8_t clientCount() const;

  static const char *AP_SSID;
  static const char *AP_PASS;

private:
  WifiState state;
  DNSServer dnsServer;

  void startAPMode();
};

// AP credentials - printed on the serial banner and shown in the dashboard.
constexpr const char *AP_SSID_DEFAULT = "ESP32-Elec-Counter";
constexpr const char *AP_PASS_DEFAULT = "configure123";
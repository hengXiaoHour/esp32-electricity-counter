#pragma once

#include <WiFi.h>
#include <DNSServer.h>
#include "../config.h"
#include "ap_creds.h"

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
// build still finds them - but nothing reads them any more. The board's OWN
// network identity (ap_ssid / ap_pass) is separate and is written at runtime;
// see WiFiManager::begin.
enum WifiState : uint8_t {
  WIFI_INIT = 0,
  WIFI_AP_MODE,
};

class NVSManager;

class WiFiManager {
public:
  // Loads the stored AP identity from NVS (falling back to the compiled
  // defaults) and brings the interface up. Takes the NVS handle because the
  // network name and password are no longer compile-time constants - they are
  // settings the user can change from the dashboard.
  void begin(NVSManager *nvsRef);
  void loop();

  // True once softAP() has returned and the dashboard server may be started.
  // Replaces the old `isConnected() || isApMode()` pair: there is only one
  // state, so a two-way test would be a lie waiting to happen.
  bool isReady() const { return state == WIFI_AP_MODE; }

  // The LIVE identity the radio is broadcasting, which is what the serial
  // banner, `wifi` and the dashboard must show. These point into WiFiManager's
  // own buffers once begin() has run, and at the compiled defaults before it.
  const char *getSSID() const { return AP_SSID; }
  const char *getPass() const { return AP_PASS; }

  // Phones/laptops currently associated with the AP. The AP-mode analogue of
  // the old STA RSSI + WS client count, and the only "is anyone watching"
  // signal that still exists.
  uint8_t clientCount() const;

  static const char *AP_SSID;
  static const char *AP_PASS;

private:
  WifiState state;
  DNSServer dnsServer;

  // Sized by the 802.11 limits in ap_creds.h (32-octet SSID, 63-octet PSK),
  // which is why begin() can reject an over-long stored value instead of
  // handing esp_wifi a truncated string.
  static char apSsid_[AP_MAX_SSID_LEN + 1];
  static char apPass_[AP_MAX_PASS_LEN + 1];

  void loadCredentials(NVSManager *nvsRef);
  void startAPMode();
};
#pragma once

#include <WiFi.h>
#include <DNSServer.h>
#include "../config.h"
#include "ap_creds.h"

// STA is the default path; the AP is fallback-only and stays OFF unless the
// home link fails. The board joins your home router, serves the dashboard from
// there, and only publishes a WiFi network of its own when it has to - so a
// working board never broadcasts anything.
//
// Boot order: try STA first (up to STA_CONNECT_TIMEOUT_MS), fall back to the AP
// if it fails or was never configured. Once the fallback AP is up it stays up
// until reboot; the next reboot tries STA first again.
//
// Two credentials, two jobs, two key pairs in NVS. The home router
// (wifi_ssid / wifi_pass) is only checked for being non-empty: a wrong password
// there costs internet access, never recovery access. The fallback AP identity
// (ap_ssid / ap_pass, settable at runtime) IS validated by ap_creds_validate()
// because an unusable PSK makes softAP() fail and takes the fallback with it.
//
// What this file still does not want back: the STA retry ladder, the
// connect-timeout reboot failsafe, the RTC boot-failure counter, link-flap
// detection, and modem-sleep eco mode.
enum WifiState : uint8_t {
  WIFI_INIT = 0,
  // Home network joined, AP off. The normal running state.
  WIFI_STA_MODE,
  // Fallback AP is up (STA failed or unconfigured). STA keeps retrying in the
  // background so a router that comes back is still joined.
  WIFI_AP_MODE,
};

class NVSManager;

class WiFiManager {
public:
  // Loads the stored home-network credentials and the fallback AP identity from
  // NVS (each falling back to its compiled default), tries STA first, and only
  // starts the fallback AP if the home link fails. Takes the NVS handle because
  // neither is a compile-time constant - they are settings the user can change
  // from the dashboard.
  void begin(NVSManager *nvsRef);
  void loop();

  // True once EITHER path is up and the dashboard server may be started:
  // STA connected (AP off) or fallback AP up. Replaces the old AP-only test.
  bool isReady() const { return state == WIFI_STA_MODE || state == WIFI_AP_MODE; }

  // True while the board is a client of the home network. This is the normal
  // running state; the AP is off when this holds (unless the fallback is up).
  bool stationUp() const { return WiFi.status() == WL_CONNECTED; }
  int8_t staRSSI() const { return stationUp() ? WiFi.RSSI() : 0; }
  const char *staSSID() const { return staSsid_; }

  // True only when the fallback AP is actually broadcasting. The dashboard and
  // `wifi` use this to say which address to open, instead of assuming the AP.
  bool apActive() const { return state == WIFI_AP_MODE; }

  // The LIVE identity the radio is broadcasting, which is what the serial
  // banner, `wifi` and the dashboard must show. These read the buffers
  // loadCredentials() fills, NOT the compiled defaults - the defaults are only
  // the fallback that function starts from. Returning AP_SSID/AP_PASS here is
  // exactly what made set_ap report success and then reboot into the old name.
  const char *getSSID() const { return apSsid_; }
  const char *getPass() const { return apPass_; }

  // Phones/laptops currently associated with the fallback AP. Zero whenever the
  // AP is off (the normal STA running state).
  uint8_t clientCount() const;

private:
  WifiState state;
  DNSServer dnsServer;

  // Sized by the 802.11 limits in ap_creds.h (32-octet SSID, 63-octet PSK),
  // which is why begin() can reject an over-long stored value instead of
  // handing esp_wifi a truncated string.
  static char apSsid_[AP_MAX_SSID_LEN + 1];
  static char apPass_[AP_MAX_PASS_LEN + 1];
  static char staSsid_[33];
  static char staPass_[64];

  void loadCredentials(NVSManager *nvsRef);
  void startStaMode();
  void startFallbackAP();
};
#include "wifi_manager.h"
#include "ap_creds.h"
#include "../utils/log_gate.h"
#include "../utils/nvs_manager.h"

// Sized from the 802.11 limits; see the comment on the declarations.
// AP_SSID_DEFAULT is a `constexpr const char *`, not a string literal, so it
// cannot be a static initialiser here - loadCredentials() copies it in as its
// very first statement, before anything can read these.
char WiFiManager::apSsid_[AP_MAX_SSID_LEN + 1] = "";
char WiFiManager::apPass_[AP_MAX_PASS_LEN + 1] = "";
char WiFiManager::staSsid_[33] = "";
char WiFiManager::staPass_[64] = "";

void WiFiManager::begin(NVSManager *nvsRef) {
  state = WIFI_INIT;
  loadCredentials(nvsRef);
  startStaMode();
}

void WiFiManager::loop() {
  // Fallback AP's captive-portal DNS only exists when the AP is up.
  if (state == WIFI_AP_MODE) {
    dnsServer.processNextRequest();
    return;
  }
  if (state != WIFI_STA_MODE) return;
  // STA-only running state lost its link: bring up the fallback AP so the
  // board is never headless. Once up it stays up until reboot; the next boot
  // tries STA first again. esp_wifi retries the STA link on its own, so this
  // only handles the "was up, now gone" transition, not the join itself.
  static uint32_t downSince = 0;
  if (WiFi.status() == WL_CONNECTED) {
    downSince = 0;
    return;
  }
  if (downSince == 0) downSince = millis();
  if (millis() - downSince > 30000) {
    STATUS_LOG("  [WiFi] home network lost for 30s - starting fallback AP\n");
    startFallbackAP();
    downSince = 0;
  }
}

uint8_t WiFiManager::clientCount() const {
  return apActive() ? WiFi.softAPgetStationNum() : 0;
}

void WiFiManager::loadCredentials(NVSManager *nvsRef) {
  // The default identity is loaded into the buffers UNCONDITIONALLY first, so
  // there is no path through this function that leaves them empty. Everything
  // below can only fail back to a usable radio.
  snprintf(apSsid_, sizeof(apSsid_), "%s", AP_SSID_DEFAULT);
  snprintf(apPass_, sizeof(apPass_), "%s", AP_PASS_DEFAULT);
  snprintf(staSsid_, sizeof(staSsid_), "%s", STA_SSID_DEFAULT);
  snprintf(staPass_, sizeof(staPass_), "%s", STA_PASS_DEFAULT);

  if (nvsRef == nullptr) return;

  // The home-network credentials get NO ap_creds_validate() pass, deliberately.
  // That function exists because a short PSK makes softAP() fail and takes the
  // dashboard down with it; a wrong station password only costs internet access,
  // and applying the AP's 8-63 octet rule would reject valid home networks for
  // no safety gain. Only "is there a name at all" is a real requirement here.
  String staSsid, staPass;
  if (nvsRef->loadWiFi(staSsid, staPass) && staSsid.length() > 0) {
    snprintf(staSsid_, sizeof(staSsid_), "%s", staSsid.c_str());
    snprintf(staPass_, sizeof(staPass_), "%s", staPass.c_str());
    DEBUG_LOG("  [NVS] home network loaded from flash: \"%s\"\n", staSsid_);
  }

  String ssid = nvsRef->loadApSsid();
  String pass = nvsRef->loadApPass();

  // NVS is written by this same firmware through ap_creds_validate() before
  // anything is committed, so a rejection here means flash holds something the
  // radio cannot use - a truncated write, a hand-edited value, or a value from a
  // future firmware with different rules. Loud, and fall back: booting with no
  // network at all would make every other repair impossible.
  const char *reason = nullptr;
  if (!ap_creds_validate(ssid.c_str(), pass.c_str(), &reason)) {
    STATUS_LOG("  [WiFi] stored AP credentials rejected (%s) — using defaults\n",
               reason ? reason : "unknown");
    return;
  }

  snprintf(apSsid_, sizeof(apSsid_), "%s", ssid.c_str());
  snprintf(apPass_, sizeof(apPass_), "%s", pass.c_str());
  DEBUG_LOG("  [NVS] AP identity loaded from flash: \"%s\"\n", apSsid_);
}

void WiFiManager::startStaMode() {
  // STA first, AP off. This is the normal running state: no broadcast, just a
  // client of the home router. The fallback AP below only starts if this fails.
  //
  // The placeholder guard compares the RUNTIME buffer, not the compiled default:
  // checking the default instead is what once made a board with real NVS
  // credentials sit quiet and never join. One literal lives in config.h as
  // STA_SSID_PLACEHOLDER; this is its only other use.
  if (staSsid_[0] == '\0' || strcmp(staSsid_, STA_SSID_PLACEHOLDER) == 0) {
    DEBUG_LOG("  [WiFi] no home network configured - starting fallback AP\n");
    startFallbackAP();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  STATUS_LOG("  [WiFi] joining home network \"%s\" ...\n", staSsid_);
  WiFi.begin(staSsid_, staPass_);

  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - t0 < (uint32_t)STA_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    state = WIFI_STA_MODE;
    STATUS_LOG("  [WiFi] joined \"%s\", IP %s - AP stays OFF\n",
               WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    return;
  }

  STATUS_LOG("  [WiFi] home network not reached in %ds - starting fallback AP\n",
             (int)(STA_CONNECT_TIMEOUT_MS / 1000));
  startFallbackAP();
}

void WiFiManager::startFallbackAP() {
  state = WIFI_AP_MODE;

  // AP+STA so the home link keeps retrying in the background while the AP is
  // up for recovery. WiFi.begin() AFTER softAP(): begin() resets the interface
  // and would tear down an AP started after it.
  WiFi.mode(WIFI_AP_STA);
  // apSsid_/apPass_, NOT the compiled defaults. Passing the defaults here is
  // what made a saved AP name look like it had worked while the board kept
  // beaconing the factory name after every reboot.
  WiFi.softAP(apSsid_, apPass_);
  WiFi.setTxPower(WIFI_POWER_21dBm);
  WiFi.setSleep(false);               // an AP must keep beaconing

  if (staSsid_[0] != '\0' && strcmp(staSsid_, STA_SSID_PLACEHOLDER) != 0) {
    WiFi.begin(staSsid_, staPass_);
  }

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  // Port 53 only - the HTTP side of the portal is the dashboard itself, served
  // by the shared AsyncWebServer on :80 (WebSocketServer::serveAsset).
  dnsServer.start(53, "*", apIP);
}
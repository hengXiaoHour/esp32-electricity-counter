#include "wifi_manager.h"
#include "ap_creds.h"
#include "../utils/log_gate.h"
#include "../utils/nvs_manager.h"

const char *WiFiManager::AP_SSID = AP_SSID_DEFAULT;
const char *WiFiManager::AP_PASS = AP_PASS_DEFAULT;

// Sized from the 802.11 limits; see the comment on the declarations.
char WiFiManager::apSsid_[AP_MAX_SSID_LEN + 1] = "";
char WiFiManager::apPass_[AP_MAX_PASS_LEN + 1] = "";

void WiFiManager::begin(NVSManager *nvsRef) {
  state = WIFI_INIT;
  loadCredentials(nvsRef);
  startAPMode();
}

void WiFiManager::loop() {
  // The captive-portal DNS wildcard is the only per-tick work left. It answers
  // every A query with 192.168.4.1, so joining the network is enough to land
  // on the dashboard no matter what URL the browser or the OS probe requests.
  if (state == WIFI_AP_MODE) {
    dnsServer.processNextRequest();
  }
}

uint8_t WiFiManager::clientCount() const {
  return (state == WIFI_AP_MODE) ? WiFi.softAPgetStationNum() : 0;
}

void WiFiManager::loadCredentials(NVSManager *nvsRef) {
  // The default identity is loaded into the buffers UNCONDITIONALLY first, so
  // there is no path through this function that leaves them empty. Everything
  // below can only fail back to a usable radio.
  snprintf(apSsid_, sizeof(apSsid_), "%s", AP_SSID_DEFAULT);
  snprintf(apPass_, sizeof(apPass_), "%s", AP_PASS_DEFAULT);

  if (nvsRef == nullptr) return;

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

void WiFiManager::startAPMode() {
  state = WIFI_AP_MODE;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setTxPower(WIFI_POWER_21dBm);  // S3 max - longest range, more heat
  WiFi.setSleep(false);               // an AP must keep beaconing

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  // Port 53 only - the HTTP side of the portal is the dashboard itself, served
  // by the shared AsyncWebServer on :80 (WebSocketServer::serveAsset).
  dnsServer.start(53, "*", apIP);
}
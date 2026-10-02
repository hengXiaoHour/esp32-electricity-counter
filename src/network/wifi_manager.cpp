#include "wifi_manager.h"
#include "../utils/log_gate.h"

const char *WiFiManager::AP_SSID = AP_SSID_DEFAULT;
const char *WiFiManager::AP_PASS = AP_PASS_DEFAULT;

void WiFiManager::begin() {
  state = WIFI_INIT;
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
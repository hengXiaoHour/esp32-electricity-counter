#include "nvs_manager.h"

void NVSManager::begin() {
  prefs.begin("elec-counter", false);
}

void NVSManager::end() {
  prefs.end();
}

// --- WiFi ---

bool NVSManager::loadWiFi(String &ssid, String &password) {
  ssid = prefs.getString("wifi_ssid", "");
  password = prefs.getString("wifi_pass", "");
  return ssid.length() > 0;
}

void NVSManager::saveWiFi(const String &ssid, const String &password) {
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", password);
}

void NVSManager::saveWiFiSSID(const String &ssid) {
  prefs.putString("wifi_ssid", ssid);
}

void NVSManager::saveWiFiPass(const String &pass) {
  prefs.putString("wifi_pass", pass);
}

void NVSManager::clearWiFi() {
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_pass");
}

void NVSManager::saveWiFiMode(uint8_t mode) {
  prefs.putUChar("wifi_mode", mode);
}

uint8_t NVSManager::loadWiFiMode() {
  return prefs.getUChar("wifi_mode", 0);
}

// --- Channel Config ---

String NVSManager::channelKey(uint8_t channel, const char *suffix) {
  String key = "ch";
  key += (channel + 1);
  key += "_";
  key += suffix;
  return key;
}

bool NVSManager::loadChannelConfig(uint8_t channel, char *name, size_t nameLen,
                                   float &currentLimit, float &powerLimit) {
  String keyName = channelKey(channel, "name");
  String keyClim = channelKey(channel, "clim");
  String keyPlim = channelKey(channel, "plim");

  String savedName = prefs.getString(keyName.c_str(), "");
  if (savedName.length() > 0) {
    savedName.toCharArray(name, nameLen);
  }

  currentLimit = prefs.getFloat(keyClim.c_str(), DEFAULT_CURRENT_LIMIT_A);
  powerLimit = prefs.getFloat(keyPlim.c_str(), DEFAULT_POWER_LIMIT_W);

  return savedName.length() > 0;
}

void NVSManager::saveChannelConfig(uint8_t channel, const char *name,
                                   float currentLimit, float powerLimit) {
  String keyName = channelKey(channel, "name");
  String keyClim = channelKey(channel, "clim");
  String keyPlim = channelKey(channel, "plim");

  prefs.putString(keyName.c_str(), name);
  prefs.putFloat(keyClim.c_str(), currentLimit);
  prefs.putFloat(keyPlim.c_str(), powerLimit);
}

// --- Calibration ---

float NVSManager::loadVoltageCalibration() {
  return prefs.getFloat("volt_cal", DEFAULT_VOLTAGE_CALIBRATION);
}

void NVSManager::saveVoltageCalibration(float value) {
  prefs.putFloat("volt_cal", value);
}

float NVSManager::loadCurrentCalibration() {
  return prefs.getFloat("curr_cal", DEFAULT_CURRENT_CALIBRATION);
}

void NVSManager::saveCurrentCalibration(float value) {
  prefs.putFloat("curr_cal", value);
}

float NVSManager::loadChannelCurrentCal(uint8_t channel) {
  String key = channelKey(channel, "ccal");
  return prefs.getFloat(key.c_str(), loadCurrentCalibration());
}

void NVSManager::saveChannelCurrentCal(uint8_t channel, float value) {
  String key = channelKey(channel, "ccal");
  prefs.putFloat(key.c_str(), value);
}

// --- Default Names ---

const char *NVSManager::defaultChannelName(uint8_t channel) {
  static const char *names[NUM_CHANNELS] = {
    "Circuit 1", "Circuit 2", "Circuit 3",
    "Circuit 4", "Circuit 5", "Circuit 6"
  };
  if (channel >= NUM_CHANNELS) return "Unknown";
  return names[channel];
}

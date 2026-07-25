#include "nvs_manager.h"

void NVSManager::begin() {
  prefs.begin("elec-counter", false);
  Serial.println("  [NVS] opened namespace 'elec-counter'");
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
  Serial.printf("  [NVS] saved WiFi: ssid=\"%s\" pass=%d chars\n", ssid.c_str(), password.length());
}

void NVSManager::saveWiFiSSID(const String &ssid) {
  prefs.putString("wifi_ssid", ssid);
  Serial.printf("  [NVS] saved WiFi SSID: \"%s\"\n", ssid.c_str());
}

void NVSManager::saveWiFiPass(const String &pass) {
  prefs.putString("wifi_pass", pass);
  Serial.printf("  [NVS] saved WiFi pass: %d chars\n", pass.length());
}

void NVSManager::clearWiFi() {
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_pass");
  Serial.println("  [NVS] cleared WiFi credentials");
}

void NVSManager::saveWiFiMode(uint8_t mode) {
  prefs.putUChar("wifi_mode", mode);
  const char *modeStr[] = {"AUTO", "STA", "AP"};
  Serial.printf("  [NVS] saved WiFi mode: %s (%d)\n",
    mode <= 2 ? modeStr[mode] : "?", mode);
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
  Serial.printf("  [NVS] ch%d config: name=\"%s\" clim=%.1fA plim=%.0fW\n",
    channel + 1, name, currentLimit, powerLimit);
}

void NVSManager::clearChannelConfig(uint8_t channel) {
  prefs.remove(channelKey(channel, "name").c_str());
  prefs.remove(channelKey(channel, "clim").c_str());
  prefs.remove(channelKey(channel, "plim").c_str());
  Serial.printf("  [NVS] ch%d config cleared (name, clim, plim)\n", channel + 1);
}

// --- Monthly kWh Limit ---

float NVSManager::loadMonthlyKwhLimit(uint8_t channel) {
  String key = channelKey(channel, "mkwh");
  return prefs.getFloat(key.c_str(), DEFAULT_MONTHLY_KWH_LIMIT);
}

void NVSManager::saveMonthlyKwhLimit(uint8_t channel, float limit) {
  String key = channelKey(channel, "mkwh");
  prefs.putFloat(key.c_str(), limit);
  Serial.printf("  [NVS] ch%d monthly kWh limit: %.1f kWh\n", channel + 1, limit);
}

// --- Calibration ---

float NVSManager::loadVoltageCalibration() {
  return prefs.getFloat("volt_cal", DEFAULT_VOLTAGE_CALIBRATION);
}

void NVSManager::saveVoltageCalibration(float value) {
  prefs.putFloat("volt_cal", value);
  Serial.printf("  [NVS] saved voltage calibration: %.1f\n", value);
}

float NVSManager::loadCurrentCalibration() {
  return prefs.getFloat("curr_cal", DEFAULT_CURRENT_CALIBRATION);
}

void NVSManager::saveCurrentCalibration(float value) {
  prefs.putFloat("curr_cal", value);
  Serial.printf("  [NVS] saved current calibration: %.1f\n", value);
}

float NVSManager::loadChannelCurrentCal(uint8_t channel) {
  String key = channelKey(channel, "ccal");
  return prefs.getFloat(key.c_str(), loadCurrentCalibration());
}

void NVSManager::saveChannelCurrentCal(uint8_t channel, float value) {
  String key = channelKey(channel, "ccal");
  prefs.putFloat(key.c_str(), value);
  Serial.printf("  [NVS] ch%d current calibration: %.1f\n", channel + 1, value);
}

// --- Relay State ---

void NVSManager::saveRelayState(uint8_t relayIndex, bool on) {
  String key = "relay_";
  key += (relayIndex + 1);
  prefs.putUChar(key.c_str(), on ? 1 : 0);
  Serial.printf("  [NVS] relay %d state: %s\n", relayIndex + 1, on ? "ON" : "OFF");
}

bool NVSManager::loadRelayState(uint8_t relayIndex, bool defaultValue) {
  String key = "relay_";
  key += (relayIndex + 1);
  return prefs.getUChar(key.c_str(), defaultValue ? 1 : 0) != 0;
}

// --- Noise Floor (zero calibration) ---

float NVSManager::loadNoiseFloor(uint8_t channel) {
  return prefs.getFloat(channelKey(channel, "nf").c_str(), 0.0f);
}

void NVSManager::saveNoiseFloor(uint8_t channel, float value) {
  prefs.putFloat(channelKey(channel, "nf").c_str(), value);
  Serial.printf("  [NVS] ch%d noise floor: %.3f A\n", channel + 1, value);
}

// --- LPF Alpha ---

float NVSManager::loadLpfAlpha(uint8_t channel) {
  return prefs.getFloat(channelKey(channel, "lpf").c_str(), 1.0f);
}

void NVSManager::saveLpfAlpha(uint8_t channel, float value) {
  prefs.putFloat(channelKey(channel, "lpf").c_str(), value);
  Serial.printf("  [NVS] ch%d LPF alpha: %.2f\n", channel + 1, value);
}

// --- Default Names ---

const char *NVSManager::defaultChannelName(uint8_t channel) {
  static const char *names[NUM_CHANNELS] = {
    "Counter 1", "Counter 2", "Counter 3",
    "Counter 4", "Counter 5", "Counter 6"
  };
  if (channel >= NUM_CHANNELS) return "Unknown";
  return names[channel];
}

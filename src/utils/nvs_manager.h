#pragma once

#include <Preferences.h>
#include "../config.h"

class NVSManager {
public:
  void begin();
  void end();

  // WiFi credentials
  bool loadWiFi(String &ssid, String &password);
  void saveWiFi(const String &ssid, const String &password);
  void saveWiFiSSID(const String &ssid);
  void saveWiFiPass(const String &pass);
  void clearWiFi();

  // WiFi mode: 0=AUTO (STA fallback AP), 1=STA only, 2=AP only
  void saveWiFiMode(uint8_t mode);
  uint8_t loadWiFiMode();

  // Per-channel configuration
  bool loadChannelConfig(uint8_t channel, char *name, size_t nameLen,
                         float &currentLimit, float &powerLimit);
  void saveChannelConfig(uint8_t channel, const char *name,
                         float currentLimit, float powerLimit);

  // Calibration
  float loadVoltageCalibration();
  void saveVoltageCalibration(float value);
  float loadCurrentCalibration();       // Legacy: single shared value
  void saveCurrentCalibration(float value); // Legacy
  float loadChannelCurrentCal(uint8_t channel);  // Per-channel
  void saveChannelCurrentCal(uint8_t channel, float value);

  // Default channel names
  static const char *defaultChannelName(uint8_t channel);

private:
  Preferences prefs;
  String channelKey(uint8_t channel, const char *suffix);
};

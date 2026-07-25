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
  void clearWiFi();

  // Per-channel configuration
  bool loadChannelConfig(uint8_t channel, char *name, size_t nameLen,
                         float &currentLimit, float &powerLimit);
  void saveChannelConfig(uint8_t channel, const char *name,
                         float currentLimit, float powerLimit);

  // Calibration
  float loadVoltageCalibration();
  void saveVoltageCalibration(float value);
  float loadCurrentCalibration();
  void saveCurrentCalibration(float value);

  // Default channel names
  static const char *defaultChannelName(uint8_t channel);

private:
  Preferences prefs;
  String channelKey(uint8_t channel, const char *suffix);
};

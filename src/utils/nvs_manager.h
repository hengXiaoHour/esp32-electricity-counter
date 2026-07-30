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

  // Per-channel name
  bool loadChannelName(uint8_t channel, char *name, size_t nameLen);
  void saveChannelName(uint8_t channel, const char *name);
  void clearChannelName(uint8_t channel);

  // Monthly kWh budget
  float loadMonthlyKwhLimit(uint8_t channel);
  void saveMonthlyKwhLimit(uint8_t channel, float limit);

  // Calibration
  float loadVoltageCalibration();
  void saveVoltageCalibration(float value);
  float loadCurrentCalibration();       // Legacy: single shared value
  void saveCurrentCalibration(float value); // Legacy
  float loadChannelCurrentCal(uint8_t channel);  // Per-channel
  void saveChannelCurrentCal(uint8_t channel, float value);

  // Relay state persistence
  void saveRelayState(uint8_t relayIndex, bool on);
  bool loadRelayState(uint8_t relayIndex, bool defaultValue);

  // Noise floor (amperes) per channel — auto-zero calibration
  float loadNoiseFloor(uint8_t channel);
  void saveNoiseFloor(uint8_t channel, float value);

  // LPF alpha per channel — 1.0 = no filtering
  float loadLpfAlpha(uint8_t channel);
  void saveLpfAlpha(uint8_t channel, float value);

  // RMS samples (runtime tunable)
  uint16_t loadRmsSamples();
  void saveRmsSamples(uint16_t value);

  // Commit pending writes to flash (required for persistence across reboot)
  void commit();

  // Default channel names
  static const char *defaultChannelName(uint8_t channel);

  // Factory reset: clear all NVS keys
  void clearAll();

private:
  Preferences prefs;
  String channelKey(uint8_t channel, const char *suffix);
};

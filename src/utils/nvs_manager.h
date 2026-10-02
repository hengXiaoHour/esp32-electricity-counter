#pragma once

#include <Preferences.h>
#include "../config.h"

class NVSManager {
public:
  void begin();
  void end();

  // --- Admin PIN -------------------------------------------------
  // Guards every mutating command. In AP-only mode the network password is
  // the ONLY thing standing between a passer-by with the dashboard open and
  // a board they can zero counters, retune CT calibration or reboot. The old
  // answer was Google sign-in plus Firebase security rules; both died with
  // the cloud, so this replaces them and - unlike the old UI-level admin
  // flag on Local mode - it is enforced here, on the ESP32.
  //
  // Plaintext on purpose: this is a soft lock on a device you already own
  // and can erase with a factory reset, not a credential protecting
  // anything of value. Hashing it would add a footgun (a bad hash bricks
  // admin access) without raising the bar meaningfully - anyone who can read
  // NVS over the AP can equally just issue commands.
  String loadPin();
  void savePin(const String &pin);
  static const char *defaultPin();

  // --- Access Point credentials ------------------------------------
  // The SSID and password the board publishes. These are NOT the old
  // station-credential keys below: wifi_ssid/wifi_pass are leftovers from a
  // firmware that joined somebody else's router, are read by nothing, and
  // `clearwifi` scrubs them. Folding the AP into those two keys would let a
  // stale router SSID silently become the network this board broadcasts.
  //
  // Loaders fall back to AP_SSID_DEFAULT / AP_PASS_DEFAULT when the key is
  // missing or empty, so a first boot needs no seeding write, and a value
  // cleared by reset_ap() reads as the factory identity rather than as "no
  // network configured".
  String loadApSsid();
  String loadApPass();
  void saveApCredentials(const String &ssid, const String &pass);
  void clearApCredentials();

  // WiFi credentials. READ-ONLY in practice: nothing writes them any more
  // (the board is AP-only). loadWiFi() exists so `wifi` can report a stale SSID
  // left by older firmware; clearWiFi() erases it.
  bool loadWiFi(String &ssid, String &password);
  void clearWiFi();

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
  float loadChannelCurrentCal(uint8_t channel);  // Per-channel
  void saveChannelCurrentCal(uint8_t channel, float value);

  // Noise floor (amperes) per channel — auto-zero calibration
  float loadNoiseFloor(uint8_t channel);
  void saveNoiseFloor(uint8_t channel, float value);

  // LPF alpha per channel — 1.0 = no filtering
  float loadLpfAlpha(uint8_t channel);
  void saveLpfAlpha(uint8_t channel, float value);

  // RMS samples (runtime tunable)
  uint16_t loadRmsSamples();
  void saveRmsSamples(uint16_t value);

  // Per-channel energy (kWh) — saved periodically to survive power loss
  float loadEnergyKWh(uint8_t channel);
  void saveEnergyKWh(uint8_t channel, float value);

  // Month (YYYYMM) of last energy rollover — monthly reset marker
  int32_t loadLastMonth();
  void saveLastMonth(int32_t month);

  // Forensic event ring: the last FORENSIC_KEEP events, persisted to flash
  // so the trail survives a reboot (RAM log is wiped; Firebase is
  // unreachable while WiFi is down). Written only for critical events
  // (trip / manual reset / rollover / inject) — a few writes a day max,
  // negligible flash wear. Restored into sysData.events in setup() and
  // from there rides the normal Firebase push on reconnect.
  static const uint8_t FORENSIC_KEEP = 10;
  void saveForensicEvents(const Event *events, uint8_t count);
  // Returns restored count (0 = nothing stored / magic mismatch).
  uint8_t loadForensicEvents(Event *out, uint8_t maxCount);

  // Commit pending writes to flash (required for persistence across reboot)
  void commit();

  // Default channel names
  static const char *defaultChannelName(uint8_t channel);

  // Factory reset: clear all NVS keys

private:
  Preferences prefs;
  String channelKey(uint8_t channel, const char *suffix);
};

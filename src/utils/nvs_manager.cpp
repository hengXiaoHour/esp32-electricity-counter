#include "nvs_manager.h"
#include "log_gate.h"

void NVSManager::begin() {
  prefs.begin("elec-counter", false);
  DEBUG_LOG("  [NVS] opened namespace 'elec-counter'\n");
}

void NVSManager::end() {
  prefs.end();
}

// --- Admin PIN ---

const char *NVSManager::defaultPin() { return "1234"; }

String NVSManager::loadPin() {
  String pin = prefs.getString("admin_pin", "");
  if (pin.length() == 0) {
    // First boot (or a board that never set one). Seed the default so the
    // key exists in NVS and the dashboard has something to compare against.
    pin = defaultPin();
    prefs.putString("admin_pin", pin);
    DEBUG_LOG("  [NVS] no admin PIN set - seeded default\n");
  }
  return pin;
}

void NVSManager::savePin(const String &pin) {
  prefs.putString("admin_pin", pin);
  DEBUG_LOG("  [NVS] admin PIN updated (%d digits)\n", (int)pin.length());
}

// --- Access Point credentials ---

String NVSManager::loadApSsid() {
  String ssid = prefs.getString("ap_ssid", "");
  if (ssid.length() == 0) {
    // Fresh board, or `reset_ap` was run. The default lives in config.h; the
    // key stays absent on purpose so the fallback is still there after a
    // firmware update that changes the default.
    return String(AP_SSID_DEFAULT);
  }
  return ssid;
}

String NVSManager::loadApPass() {
  String pass = prefs.getString("ap_pass", "");
  if (pass.length() == 0) return String(AP_PASS_DEFAULT);
  return pass;
}

void NVSManager::saveApCredentials(const String &ssid, const String &pass) {
  // NOTE: staged only. The caller MUST call commit() or the board keeps
  // broadcasting the old identity across its restart - which is exactly what
  // processCommand()'s trailing `if (handled) nvs->commit();` is for.
  prefs.putString("ap_ssid", ssid);
  prefs.putString("ap_pass", pass);
  DEBUG_LOG("  [NVS] AP credentials staged: ssid=\"%s\" pass=%d chars\n",
            ssid.c_str(), (int)pass.length());
}

void NVSManager::clearApCredentials() {
  prefs.remove("ap_ssid");
  prefs.remove("ap_pass");
  DEBUG_LOG("  [NVS] AP credentials cleared — defaults apply after restart\n");
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
  // No commit() here: Preferences' own commit is NVSManager::commit(), and the
  // caller holds dataMutex around it (same rule as saveApCredentials).
  DEBUG_LOG("  [NVS] station WiFi written: \"%s\"\n", ssid.c_str());
}

void NVSManager::clearWiFi() {
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_pass");
  // wifi_mode was written by pre-AP-only firmware and has been read by nothing
  // since. Erase it too, so `clearwifi` really does leave no WiFi state behind.
  prefs.remove("wifi_mode");
  DEBUG_LOG("  [NVS] cleared WiFi credentials + mode\n");
}

// --- Channel Config ---

String NVSManager::channelKey(uint8_t channel, const char *suffix) {
  String key = "ch";
  key += (channel + 1);
  key += "_";
  key += suffix;
  return key;
}

bool NVSManager::loadChannelName(uint8_t channel, char *name, size_t nameLen) {
  String keyName = channelKey(channel, "name");
  String savedName = prefs.getString(keyName.c_str(), "");
  if (savedName.length() > 0) {
    savedName.toCharArray(name, nameLen);
  }
  return savedName.length() > 0;
}

void NVSManager::saveChannelName(uint8_t channel, const char *name) {
  String keyName = channelKey(channel, "name");
  prefs.putString(keyName.c_str(), name);
  DEBUG_LOG("  [NVS] ch%d name: \"%s\"\n", channel + 1, name);
}

void NVSManager::clearChannelName(uint8_t channel) {
  prefs.remove(channelKey(channel, "name").c_str());
  DEBUG_LOG("  [NVS] ch%d name cleared\n", channel + 1);
}

// --- Monthly kWh Limit ---

float NVSManager::loadMonthlyKwhLimit(uint8_t channel) {
  String key = channelKey(channel, "mkwh");
  return prefs.getFloat(key.c_str(), DEFAULT_MONTHLY_KWH_LIMIT);
}

void NVSManager::saveMonthlyKwhLimit(uint8_t channel, float limit) {
  String key = channelKey(channel, "mkwh");
  prefs.putFloat(key.c_str(), limit);
  DEBUG_LOG("  [NVS] ch%d monthly kWh limit: %.1f kWh\n", channel + 1, limit);
}

// --- Calibration (NVS-backed) ---

float NVSManager::loadVoltageCalibration() {
  return prefs.getFloat("volt_cal", DEFAULT_VOLTAGE_CALIBRATION);
}

void NVSManager::saveVoltageCalibration(float value) {
  prefs.putFloat("volt_cal", value);
  DEBUG_LOG("  [NVS] saved voltage calibration: %.1f\n", value);
}

float NVSManager::loadCurrentCalibration() {
  return prefs.getFloat("curr_cal", DEFAULT_CURRENT_CALIBRATION);
}

float NVSManager::loadChannelCurrentCal(uint8_t channel) {
  String key = channelKey(channel, "ccal");
  float v = prefs.getFloat(key.c_str(), NAN);
  if (isnan(v)) return loadCurrentCalibration();
  return v;
}

void NVSManager::saveChannelCurrentCal(uint8_t channel, float value) {
  String key = channelKey(channel, "ccal");
  prefs.putFloat(key.c_str(), value);
  DEBUG_LOG("  [NVS] ch%d current calibration: %.1f\n", channel + 1, value);
}

// --- Noise Floor ---

float NVSManager::loadNoiseFloor(uint8_t channel) {
  String key = channelKey(channel, "nf");
  return prefs.getFloat(key.c_str(), 0.0f);
}

void NVSManager::saveNoiseFloor(uint8_t channel, float value) {
  String key = channelKey(channel, "nf");
  prefs.putFloat(key.c_str(), value);
  DEBUG_LOG("  [NVS] ch%d noise floor: %.3f A\n", channel + 1, value);
}

// --- LED type ---

bool NVSManager::loadLedType() {
  // Default normal (non-RGB) LED; key absent on a fresh board.
  return prefs.getBool("led_rgb", false);
}

void NVSManager::saveLedType(bool rgb) {
  prefs.putBool("led_rgb", rgb);
  DEBUG_LOG("  [NVS] LED type staged: %s\n", rgb ? "rgb" : "normal");
}

// --- LPF Alpha ---

float NVSManager::loadLpfAlpha(uint8_t channel) {
  String key = channelKey(channel, "lpf");
  return prefs.getFloat(key.c_str(), DEFAULT_LPF_ALPHA);
}

void NVSManager::saveLpfAlpha(uint8_t channel, float value) {
  String key = channelKey(channel, "lpf");
  prefs.putFloat(key.c_str(), value);
  DEBUG_LOG("  [NVS] ch%d LPF alpha: %.2f\n", channel + 1, value);
}

// --- RMS Samples ---

uint16_t NVSManager::loadRmsSamples() {
  return (uint16_t)prefs.getFloat("rms_samp", (float)MAX_RMS_SAMPLES / 2);
}

void NVSManager::saveRmsSamples(uint16_t value) {
  prefs.putFloat("rms_samp", (float)value);
  DEBUG_LOG("  [NVS] RMS samples: %d\n", value);
}

// --- AZ Batches ---

uint8_t NVSManager::loadAzBatches() {
  float v = prefs.getFloat("az_batch", 1.0f);
  int n = (int)v;
  if (n < 1) n = 1;
  if (n > 64) n = 64;
  return (uint8_t)n;
}

void NVSManager::saveAzBatches(uint8_t value) {
  prefs.putFloat("az_batch", (float)value);
  DEBUG_LOG("  [NVS] auto-zero batches: %d\n", value);
}

// --- Default Names ---

const char *NVSManager::defaultChannelName(uint8_t channel) {
  static const char *names[NUM_CHANNELS] = {
    "Counter 1", "Counter 2", "Counter 3",
    "Counter 4", "Counter 5"
  };
  if (channel >= NUM_CHANNELS) return "Unknown";
  return names[channel];
}

// --- Commit ---

void NVSManager::commit() {
  prefs.end();
  prefs.begin("elec-counter", false);
}

// --- Per-channel energy (kWh) persistence ---

float NVSManager::loadEnergyKWh(uint8_t channel) {
  String key = channelKey(channel, "kwh");
  return prefs.getFloat(key.c_str(), 0.0f);
}

void NVSManager::saveEnergyKWh(uint8_t channel, float value) {
  String key = channelKey(channel, "kwh");
  prefs.putFloat(key.c_str(), value);
}

// --- Monthly rollover marker ---

int32_t NVSManager::loadLastMonth() {
  return prefs.getLong("last_month", 0);
}

void NVSManager::saveLastMonth(int32_t month) {
  prefs.putLong("last_month", month);
  DEBUG_LOG("  [NVS] saved last month: %ld\n", (long)month);
}

// --- Forensic event ring (reboot-proof trail) ---

#define FORENSIC_MAGIC 0x46565231UL  // "FVR1"
#define FORENSIC_KEY "fev_ring"

struct ForensicBlob {
  uint32_t magic;
  uint8_t count;
  Event slots[NVSManager::FORENSIC_KEEP];
};

void NVSManager::saveForensicEvents(const Event *events, uint8_t count) {
  if (!events || count == 0) return;
  if (count > FORENSIC_KEEP) count = FORENSIC_KEEP;
  ForensicBlob blob;
  blob.magic = FORENSIC_MAGIC;
  blob.count = count;
  // Caller passes the TAIL of the RAM ring; store in chronological order.
  memcpy(blob.slots, events, count * sizeof(Event));
  prefs.putBytes(FORENSIC_KEY, &blob, sizeof(uint32_t) + sizeof(uint8_t) + count * sizeof(Event));
}

uint8_t NVSManager::loadForensicEvents(Event *out, uint8_t maxCount) {
  if (!out || maxCount == 0) return 0;
  ForensicBlob blob;
  memset(&blob, 0, sizeof(blob));
  size_t n = prefs.getBytes(FORENSIC_KEY, &blob, sizeof(blob));
  if (n < sizeof(uint32_t) + sizeof(uint8_t)) return 0;
  if (blob.magic != FORENSIC_MAGIC) return 0;
  if (blob.count == 0 || blob.count > FORENSIC_KEEP) return 0;
  if (n < sizeof(uint32_t) + sizeof(uint8_t) + blob.count * sizeof(Event)) return 0;
  uint8_t keep = (blob.count < maxCount) ? blob.count : maxCount;
  memcpy(out, blob.slots, keep * sizeof(Event));
  return keep;
}

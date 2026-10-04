#include "system_json.h"
#include "../core/power_calculator.h"
#include "../utils/nvs_manager.h"
#include "time_sync.h"

#include <time.h>

// Appends `s` as a JSON string BODY (no surrounding quotes), escaping the
// characters that would otherwise end the string or escape a quote.
//
// Not theoretical: ap_creds_validateSsid rejects control bytes, length and
// surrounding spaces, but it accepts `"` and `\`. A name like
// Ben "the meter"\ Lab is legal for the radio and would have produced invalid
// JSON that JSON.parse() throws away - taking the whole broadcast, every live
// reading included, with it.
static void appendJsonEscaped(String &json, const String &s) {
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '"' || c == '\\') {
      json += '\\';
      json += c;
    } else if ((unsigned char)c < 0x20) {
      // Unreachable for a validated SSID, but an unescaped control byte would
      // make the frame unparseable, so never emit one.
      json += ' ';
    } else {
      json += c;
    }
  }
}

// Serialises SystemData + the live calibration/auto-zero fields held by
// PowerCalculator into the dashboard's wire format.
//
// Field names are deliberately short: this string is rebuilt and pushed to
// every connected browser 6-7 times a second, and the dashboard keeps six
// 240-sample history buffers keyed by the same names.
//
// Reads nvs->loadLastMonth() on every call.
// That is a flash read per broadcast; it was equally true before this moved,
// and the NVS cache keeps it cheap.
void buildSystemJson(const SystemData &data, PowerCalculator *powerCalc,
                     NVSManager *nvs, String &json) {
  json = "{\"v\":";
  json += String(data.voltageRMS, 1);
  json += ",\"uptime\":";
  json += data.uptime;
  // On-die temperature, one decimal. NAN (no sensor on this chip) is emitted
  // as JSON null, never as nan - String(NAN) would print "nan", which is not
  // valid JSON and would make the dashboard drop the whole frame.
  json += ",\"mcuTemp\":";
  if (isnan(data.mcuTempC)) json += "null";
  else json += String(data.mcuTempC, 1);
  json += ",\"eco\":";
  json += data.ecoMode ? "true" : "false";
  json += ",\"wifi\":";
  json += data.wifiConnected ? "true" : "false";
  json += ",\"rssi\":";
  json += data.wifiRSSI;
  json += ",\"ap\":";
  json += data.apMode ? "true" : "false";
  json += ",\"voltageCalibration\":";
  json += String(data.voltageCalibration, 1);
  json += ",\"currentCalibration\":[";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    json += String(data.currentCalibration[i], 1);
    if (i < NUM_CHANNELS - 1) json += ",";
  }
  json += "]";
  json += ",\"rmsSamples\":";
  json += data.rmsSamples;
  json += ",\"azBatches\":";
  json += powerCalc->azBatches;
  json += ",\"noiseFloor\":[";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    json += String(powerCalc->noiseFloor[i], 3);
    if (i < NUM_CHANNELS - 1) json += ",";
  }
  json += "]";
  json += ",\"azActive\":";
  json += powerCalc->isAutoZeroActive() ? "true" : "false";
  json += ",\"azChannel\":";
  json += powerCalc->getAutoZeroChannel();
  json += ",\"azProgress\":";
  json += powerCalc->getAutoZeroProgress();
  json += ",\"azQueue\":[";
  {
    int q[NUM_CHANNELS];
    int qLen = powerCalc->getAutoZeroQueue(q, NUM_CHANNELS);
    for (int i = 0; i < qLen; i++) {
      json += q[i];
      if (i < qLen - 1) json += ",";
    }
  }
  json += "]";
  json += ",\"lpfAlpha\":[";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    json += String(powerCalc->lpfAlpha[i], 2);
    if (i < NUM_CHANNELS - 1) json += ",";
  }
  json += "]";
  json += ",\"firmwareVersion\":\"";
  json += FIRMWARE_VERSION;
  json += "\",\"epoch\":";
  json += (long)time(nullptr);
  json += ",\"lastMonth\":";
  json += nvs->loadLastMonth();
  // The AP identity actually in effect, so the dashboard can show the user what
  // to edit instead of a placeholder. Without this the Access Point panel always
  // looked like a fresh board, whatever was stored - you could rename the
  // network, come back, and have no way to tell what it was called now.
  //
  // The SSID is not a secret (a WiFi scan reads it off the air). The PASSWORD is
  // never sent: `apIsDefault` says only whether both values still match the
  // factory ones, which is what the UI needs to word its placeholder.
  {
    String ssid = nvs->loadApSsid();
    String pass = nvs->loadApPass();
    json += ",\"apSsid\":\"";
    appendJsonEscaped(json, ssid);
    json += "\",\"apIsDefault\":";
    json += (ssid == AP_SSID_DEFAULT && pass == AP_PASS_DEFAULT) ? "true" : "false";
  }
  // The home network, same rules: the SSID is public (a scan reads it), the
  // password never leaves the board. `staIsDefault` is false whenever anything
  // is stored, which is what the UI needs to word its placeholder.
  {
    String ssid, pass;
    const bool stored = nvs->loadWiFi(ssid, pass) && ssid.length() > 0;
    json += ",\"staSsid\":\"";
    appendJsonEscaped(json, ssid);
    json += "\",\"staIsDefault\":";
    json += stored ? "false" : "true";
  }
  // Clock health. Read straight from TimeSync rather than through SystemData:
  // it is owned by the network layer, needs no mutex, and must be visible even
  // when it has never been set (which is the case worth showing).
  json += ",\"time\":{\"ok\":";
  json += timeSync.isSynced() ? "true" : "false";
  json += ",\"age\":";
  json += (long)timeSync.secondsSinceSync();
  json += "}";
  json += ",\"ch\":[";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    const ChannelData &ch = data.channels[i];
    json += "{\"n\":\"";
    json += ch.name;
    json += "\",\"a\":";
    json += String(ch.currentRMS, 2);
    json += ",\"w\":";
    json += String(ch.activePower, 1);
    json += ",\"va\":";
    json += String(ch.apparentPower, 1);
    json += ",\"pf\":";
    json += String(ch.powerFactor, 3);
    json += ",\"kwh\":";
    json += String(ch.energyKWh, 3);
    json += ",\"s\":";
    json += ch.status;
    json += ",\"mkwh\":";
    json += String(ch.monthlyKwhLimit, 1);
    json += "}";
    if (i < NUM_CHANNELS - 1) json += ",";
  }

  json += "],\"events\":[";

  int start = data.eventCount > 10 ? data.eventCount - 10 : 0;
  for (int i = start; i < data.eventCount; i++) {
    const Event &ev = data.events[i];
    json += "{\"t\":";
    json += ev.timestamp;
    json += ",\"c\":";
    json += ev.channel;
    json += ",\"s\":";
    json += ev.status;
    json += ",\"v\":";
    json += String(ev.value, 1);
    json += ",\"m\":\"";
    json += ev.message;
    json += "\"}";
    if (i < data.eventCount - 1) json += ",";
  }

  json += "]}";
}
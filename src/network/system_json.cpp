#include "system_json.h"
#include "../core/power_calculator.h"
#include "../utils/nvs_manager.h"

#include <time.h>

// Serialises SystemData + the live calibration/auto-zero fields held by
// PowerCalculator into the dashboard's wire format.
//
// Field names are deliberately short: this string is rebuilt and pushed to
// every connected browser 6-7 times a second, and the dashboard keeps six
// 240-sample history buffers keyed by the same names.
//
// Reads nvs->loadLastMonth()/loadNtfyTopic()/loadNtfyEnabled() on every call.
// That is a flash read per broadcast; it was equally true before this moved,
// and the NVS cache keeps it cheap.
void buildSystemJson(const SystemData &data, PowerCalculator *powerCalc,
                     NVSManager *nvs, String &json) {
  json = "{\"v\":";
  json += String(data.voltageRMS, 1);
  json += ",\"uptime\":";
  json += data.uptime;
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
  json += ",\"ntfy\":{\"topic\":\"";
  json += nvs->loadNtfyTopic();
  json += "\",\"enabled\":";
  json += nvs->loadNtfyEnabled() ? "true" : "false";
  json += "},\"ch\":[";
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
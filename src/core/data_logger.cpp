#include "data_logger.h"

DataLogger logger;

void DataLogger::begin() {
  lastLogTime = 0;
  fsReady = false;

  if (!LittleFS.begin()) {
    LittleFS.format();
    if (!LittleFS.begin()) return;
  }

  if (!LittleFS.exists(LOG_DIR)) {
    LittleFS.mkdir(LOG_DIR);
  }

  fsReady = true;
}

void DataLogger::log(const SystemData &data) {
  if (!fsReady) return;

  uint32_t now = millis();
  if (now - lastLogTime < LOG_INTERVAL_MS) return;
  lastLogTime = now;

  String path = getDatePath();
  bool exists = LittleFS.exists(path);

  File f = LittleFS.open(path, "a");
  if (!f) return;

  if (!exists || f.size() == 0) {
    writeHeader(f);
    Serial.printf("  [LOG] created %s\n", path.c_str());
  }

  String row;
  row += now / 1000;
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    row += ",";
    row += String(data.channels[ch].currentRMS, 3);
  }
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    row += ",";
    row += String(data.channels[ch].energyKWh, 3);
  }
  for (int ch = 0; ch < RELAY_CHANNEL_COUNT; ch++) {
    row += ",";
    row += data.channels[ch].relayOn ? "1" : "0";
  }
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    row += ",";
    row += data.channels[ch].status;
  }
  row += ",";
  row += String(data.voltageRMS, 1);
  row += "\n";

  f.print(row);
  f.close();

  Serial.printf("  [LOG] wrote %d bytes → %s\n", row.length(), path.c_str());

  checkRetention();
}

String DataLogger::getDatePath() {
  unsigned long t = millis() / 1000;
  unsigned long days = t / 86400;
  unsigned int year = 2026;
  unsigned int month = 7;
  unsigned int day = 25 + days;
  while (day > 30) { day -= 30; month++; }
  while (month > 12) { month -= 12; year++; }
  char buf[32];
  snprintf(buf, sizeof(buf), "%s/%04u-%02u-%02u.csv", LOG_DIR, year, month, day);
  return String(buf);
}

void DataLogger::writeHeader(File &f) {
  f.print("ts");
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    f.print(",ch");
    f.print(ch + 1);
    f.print("_a");
  }
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    f.print(",ch");
    f.print(ch + 1);
    f.print("_kwh");
  }
  for (int ch = 0; ch < RELAY_CHANNEL_COUNT; ch++) {
    f.print(",ch");
    f.print(ch + 1);
    f.print("_rly");
  }
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    f.print(",ch");
    f.print(ch + 1);
    f.print("_st");
  }
  f.print(",v\n");
}

void DataLogger::checkRetention() {
  unsigned long totalSpace = LittleFS.totalBytes();
  unsigned long usedSpace = LittleFS.usedBytes();
  unsigned long freeSpace = totalSpace - usedSpace;

  if (freeSpace > totalSpace / 5) return;

  File root = LittleFS.open(LOG_DIR);
  if (!root) return;

  String oldest;
  File f;
  while ((f = root.openNextFile())) {
    String name = f.name();
    f.close();
    if (!name.endsWith(".csv")) continue;
    if (oldest.isEmpty() || name < oldest) {
      oldest = name;
    }
  }
  root.close();

  if (!oldest.isEmpty()) {
    String delPath = String(LOG_DIR) + "/" + oldest;
    LittleFS.remove(delPath);
    Serial.printf("  [LOG] retention: deleted %s (free space < 20%%)\n", delPath.c_str());
  }
}

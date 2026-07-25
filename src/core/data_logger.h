#pragma once

#include <LittleFS.h>
#include "../config.h"

#define RETENTION_DAYS   30

class DataLogger {
public:
  void begin();
  void log(const SystemData &data);

private:
  uint32_t lastLogTime;
  bool fsReady;

  String getDatePath();
  void writeHeader(File &f);
  void checkRetention();
};

extern DataLogger logger;

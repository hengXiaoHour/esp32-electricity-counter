#pragma once

#include <Arduino.h>
#include "../config.h"

class NVSManager;
class PowerCalculator;
class LimitManager;
class Buzzer;
class WiFiManager;
class OTAHandler;

// Executes textual diagnostic commands (the serial console vocabulary) and
// captures their output into a String instead of writing to Serial. This lets
// the same command set be driven from the web UI over WebSocket or Firebase.
//
// Only the remote-safe subset is exposed: status, ch, cal, info, wifi, help,
// buzz, inject, reset, reset_name. Blocking or destructive commands
// (test led, nvs_debug, reboot, setwifi connect/save, clearwifi) stay
// serial-only and are handled in the sketch.
class ConsoleHandler {
public:
  ConsoleHandler();

  void begin(NVSManager *nvs, PowerCalculator *powerCalc, SystemData *sysData,
             SemaphoreHandle_t *dataMutex, Buzzer *buzzer, LimitManager *limitMgr,
             WiFiManager *wifiMgr, OTAHandler *otaHandler);

  // Runs one command line, appending its output (newline separated) to `out`.
  // Unknown commands append "Unknown command. Type 'help'.".
  void exec(const String &line, String &out);

  // True once begin() has been called with the required references.
  bool isReady() const { return ready; }

  // Hard cap on a single response so a runaway command can't blow up the
  // WebSocket frame / RTDB write.
  static const size_t MAX_OUTPUT = 3072;

private:
  NVSManager *nvs;
  PowerCalculator *powerCalc;
  SystemData *sysData;
  SemaphoreHandle_t *dataMutex;
  Buzzer *buzzer;
  LimitManager *limitMgr;
  WiFiManager *wifiMgr;
  OTAHandler *otaHandler;
  bool ready;

  void cmdStatus(String &out);
  void cmdChannel(int ch, String &out);
  void cmdCal(String &out);
  void cmdInfo(String &out);
  void cmdWifi(String &out);
  void cmdHelp(String &out);
};

// Escapes a text blob for embedding inside a JSON string literal.
String consoleJsonEscape(const String &s);

// Appends printf-formatted text plus a newline to `out`, respecting
// ConsoleHandler::MAX_OUTPUT.
void consoleAppendf(String &out, const char *fmt, ...);

extern ConsoleHandler consoleHandler;

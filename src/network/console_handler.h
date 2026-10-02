#pragma once

#include <Arduino.h>
#include "../config.h"

class NVSManager;
class PowerCalculator;
class LimitManager;
class Buzzer;
class WiFiManager;
class OTAHandler;
class StatusLED;

// Executes textual diagnostic commands (the serial console vocabulary) and
// captures their output into a String instead of writing to Serial. This lets
// the same command set be driven from the web UI over WebSocket or Firebase.
//
// Commands that would block the network task (test led) or are destructive
// (reboot, nvs_debug, setwifi connect/save, clearwifi) are flagged as
// deferred — the web console sets pendingDefer and the sketch's loop()
// runs them on Core 1 where blocking is safe.
class ConsoleHandler {
public:
  ConsoleHandler();

  void begin(NVSManager *nvs, PowerCalculator *powerCalc, SystemData *sysData,
             SemaphoreHandle_t *dataMutex, Buzzer *buzzer, LimitManager *limitMgr,
             WiFiManager *wifiMgr, OTAHandler *otaHandler, StatusLED *statusLed);

  // Runs one command line, appending its output (newline separated) to `out`.
  // Unknown commands append "Unknown command. Type 'help'.".
  // Blocking/destructive commands are deferred — sets pendingDefer and returns
  // an "acknowledged" message; the sketch's loop() calls runDeferred().
  void exec(const String &line, String &out);

  // Drives non-blocking operations (test led state machine). Call every loop().
  void loop();

  // Runs a deferred command (called from loop() on Core 1). Captures output
  // into pendingOutput; sketch can forward it to the WS/FB response path.
  void runDeferred();

  // True once begin() has been called with the required references.
  bool isReady() const { return ready; }

  // True if a deferred command was requested but not yet run.
  bool hasDeferred() const { return pendingDefer; }

  // True if test led is currently running (driven by loop()).
  bool isLedTestRunning() const { return ledTestActive; }

  // Returns and clears accumulated deferred output (for WS/FB response).
  String takePendingOutput();

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
  StatusLED *statusLed;
  bool ready;

  // Deferred command (blocking/destructive — runs from loop()).
  bool pendingDefer;
  String pendingCmd;
  String pendingOutput;

  // Non-blocking test led state machine.
  bool ledTestActive;
  int ledTestPhase;
  uint32_t ledTestNextStep;

  void cmdStatus(String &out);
  void cmdChannel(int ch, String &out);
  void cmdCal(String &out);
  void cmdInfo(String &out);
  void cmdWifi(String &out);
  void cmdHelp(String &out);
  void cmdLedTest(String &out);
  void cmdNvsDebug(String &out);
  void cmdReboot(String &out);
  void cmdRmsSamples(const String &args, String &out);
  void cmdCurrCal(const String &args, String &out);
  void cmdAutoZero(const String &args, String &out);
  void cmdVoltCal(const String &args, String &out);
  void cmdClearWifi(String &out);

  // Push RAM energy counters to NVS + commit. Called before every
  // deliberate restart (reboot, setwifi connect) so counter data survives.
  void flushEnergy();

  static void ledTestStep(ConsoleHandler *self);
};

// Escapes a text blob for embedding inside a JSON string literal.
String consoleJsonEscape(const String &s);

// Appends printf-formatted text plus a newline to `out`, respecting
// ConsoleHandler::MAX_OUTPUT.
void consoleAppendf(String &out, const char *fmt, ...);

extern ConsoleHandler consoleHandler;

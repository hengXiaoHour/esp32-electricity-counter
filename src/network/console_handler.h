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
class CloudPush;

// Executes textual diagnostic commands (the serial console vocabulary) and
// captures their output into a String instead of writing to Serial. This lets
// the same command set be driven from the web UI over WebSocket.
//
// Commands that would block the network task (test led) or are destructive
// (reboot, nvs_debug) are flagged as deferred — the web console sets
// pendingDefer and the sketch's loop() runs them on Core 1 where blocking is
// safe. Note that clearwifi is NOT deferred - it runs inline and takes effect
// on the next boot - while setwifi DOES defer its reboot, because the commit
// has to be safely in flash before the board restarts.
class ConsoleHandler {
public:
  ConsoleHandler();

  void begin(NVSManager *nvs, PowerCalculator *powerCalc, SystemData *sysData,
             SemaphoreHandle_t *dataMutex, Buzzer *buzzer, LimitManager *limitMgr,
             WiFiManager *wifiMgr, OTAHandler *otaHandler, StatusLED *statusLed,
             CloudPush *cloudPush);

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

  // Asks for a restart without blocking the caller. `reason` (may be NULL) is
  // printed just before the reboot runs, so the last thing on the serial log is
  // why the board went down - which for set_ap / reset_ap is the one thing the
  // user needs to know when the phone drops off the network.
  //
  // Deferred rather than immediate on purpose: the command that asks for this
  // usually just changed the SSID, and rebooting inside the WebSocket handler
  // would kill the connection before the acknowledgement is written.
  void requestReboot(const char *reason = nullptr);

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
  CloudPush *cloud;
  bool ready;

  // Deferred command (blocking/destructive — runs from loop()).
  // Written on Core 0 (serial/WS console via exec/requestReboot), consumed
  // on Core 1 (loop() -> runDeferred). Volatile: without it the Core-1 read
  // can sit in a register and a deferred reboot silently never fires.
  volatile bool pendingDefer;
  String pendingCmd;
  String pendingOutput;
  String pendingRebootNote;

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
  void cmdNvsDebug(String &out);
  void cmdReboot(String &out);
  void cmdRmsSamples(const String &args, String &out);
  void cmdAzBatches(const String &args, String &out);
  void cmdCurrCal(const String &args, String &out);
  void cmdAutoZero(const String &args, String &out);
  void cmdVoltCal(const String &args, String &out);
  void cmdClearWifi(String &out);
  void cmdSetWifi(const String &args, String &out);
  void cmdSetCloud(const String &args, String &out);
  void cmdClearCloud(String &out);
  void cmdCloudDiag(String &out);
  void cmdOta(const String &args, String &out);
  void cmdUpdate(String &out);
  void cmdSetAp(const String &args, String &out);
  void cmdResetAp(String &out);
  void cmdResetDay(const String &args, String &out);
  void cmdLed(const String &args, String &out);

  // Push RAM energy counters to NVS + commit. Called before every
  // deliberate restart (reboot, set_ap, setwifi) so counter data survives.
  void flushEnergy();

  static void ledTestStep(ConsoleHandler *self);
};

// Task diagnostics for `info` (defined in the sketch, set in setup()).
// g_loopIters proves the Core-1 Arduino loop is alive; the handles feed
// uxTaskGetStackHighWaterMark so a silent loopTask death shows up as a
// number instead of a mystery.
extern volatile uint32_t g_loopIters;
extern TaskHandle_t g_networkTask;
extern TaskHandle_t g_sensorTask;

// Escapes a text blob for embedding inside a JSON string literal.
String consoleJsonEscape(const String &s);

// Appends printf-formatted text plus a newline to `out`, respecting
// ConsoleHandler::MAX_OUTPUT.
void consoleAppendf(String &out, const char *fmt, ...);

extern ConsoleHandler consoleHandler;

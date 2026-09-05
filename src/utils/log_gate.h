#pragma once

#include <Arduino.h>

// Serial stream toggles, driven by the `status` and `debug` console commands
// (serial or web console — shared engine). Both default to OFF so a fresh
// boot prints only the WiFi/server block.
//
// STATUS_LOG: operational transitions worth watching live — eco sleep,
// cloud viewer presence, bridge start, rollover, auto-zero, ntfy pushes.
// DEBUG_LOG: developer diagnostics — NVS save confirmations, [CMD] traces,
// Firebase/NTFY error details, boot calibration dumps.
extern bool g_statusStream;
extern bool g_debugStream;

#define STATUS_LOG(...) \
  do {                  \
    if (g_statusStream) Serial.printf(__VA_ARGS__); \
  } while (0)
#define DEBUG_LOG(...) \
  do {                 \
    if (g_debugStream) Serial.printf(__VA_ARGS__); \
  } while (0)

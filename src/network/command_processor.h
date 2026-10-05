#pragma once

#include "../config.h"
#include "../utils/nvs_manager.h"
#include "../core/power_calculator.h"

class LimitManager;
class SystemData;

// Processes a dashboard command JSON string (e.g. {"cmd":"set_name","ch":0,...}).
// The WebSocket server is the usual caller; CloudPush::pollCmd is the other
// one (remote downlink from the realtime database).
//
// Returns true if a command was matched AND executed (the caller may then force
// an immediate broadcast), false otherwise.
//
// AUTHENTICATION
//   Every mutating verb requires {"pin":"<admin pin>"} in the frame. Two verbs
//   are exempt: `set_time` (sent automatically by any viewer that connects -
//   it is what gives the board a clock at all) and `verify_pin` (which exists to
//   answer the question). The PIN is enforced HERE, on the ESP32, not in the
//   dashboard: the previous UI-level admin flag was trivially bypassed and the
//   Firebase rules that used to back it are deleted.
//
//   skipAuth (default false): when true, the PIN gate below is SKIPPED
//   entirely. Only CloudPush::pollCmd may pass true - its trust comes from
//   the RTDB rules (only the admin Gmail can write cmd) + TLS + the board's
//   own ID token, and the cloud never carries a PIN. Local WebSocket callers
//   use the default and stay PIN-gated with zero behavior change.
//
// responseOut (optional): when non-null, text-producing commands (currently
// {"cmd":"console","line":"..."} and the PIN replies) append their output here
// so the caller can ship it back to the requesting client. Pass nullptr to
// discard output.
//
// authRejected (optional): set to true when the command was well-formed but
// blocked for a missing/wrong PIN. This is deliberately separate from the return
// value, so the caller can tell "you are not allowed" from "I did not
// understand that" and answer the client differently.
bool processCommand(NVSManager *nvs, SystemData *sysData,
                    SemaphoreHandle_t *dataMutex,
                    PowerCalculator *powerCalc, LimitManager *limitMgr,
                    const char *msg, String *responseOut = nullptr,
                    bool *authRejected = nullptr, bool skipAuth = false);
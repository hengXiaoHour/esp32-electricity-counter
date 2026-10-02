#pragma once

#include <stdbool.h>
#include <stddef.h>

// Admin-PIN gate — the security boundary of the AP-only board.
//
// Deliberately free of Arduino headers (no String, no Preferences, no WiFi) so
// it can be compiled and unit-tested on the host with scripts/test_auth_gate.c.
// A security check that can only be exercised by flashing hardware is a check
// nobody runs; this one has a negative-control test suite.
//
// Two verbs are exempt from the PIN: `set_time` (sent automatically by any
// viewer that connects — it is what gives the board a clock at all, and gating
// it would leave the monthly rollover dormant until somebody unlocked the UI)
// and `verify_pin` (whose entire job is to answer the question).

#define AUTH_MAX_VERB 32
#define AUTH_MAX_PIN  64

// True if `verb` is one of the exempt verbs. Unknown verbs are NOT exempt:
// anything unrecognised is treated as potentially mutating.
bool auth_isExemptVerb(const char *verb);

// Copies the value of "pin":"..." from `frame` into `out` (NUL-terminated).
// Returns false when the field is absent or unterminated.
// JSON-aware for the escape sequences that can legitimately appear in a PIN
// (\\ \" \/ \n \t \r), so a PIN containing a backslash still round-trips.
bool auth_extractPin(const char *frame, char *out, size_t outLen);

// Core decision. Returns true when `frame` may proceed, false when it must be
// rejected for a missing or wrong PIN.
//
// `outVerb` (optional) receives the command verb for logging.
bool auth_check(const char *frame, const char *expectedPin, char *outVerb,
                size_t outVerbLen);
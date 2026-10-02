#pragma once

#include <stdbool.h>
#include <stddef.h>

// WPA2 soft-AP credential rules, in one place, because both the serial console
// and the WebSocket command path must agree on them - and because getting them
// wrong does not produce an error message, it produces a board you can no
// longer reach.
//
// Why this is a separate, Arduino-free module
// -------------------------------------------
// The AP's SSID and password are now writable at runtime. That turns a bad value
// into a lockout: `esp_wifi_set_config()` rejects a PSK shorter than 8 octets,
// softAP() then returns false, and the board finishes booting with no radio at
// all. The only way back is the serial port. So the check has to be exhaustive
// BEFORE anything is written to flash, and - following auth_gate.h - it lives
// where it can be exercised on the host by scripts/test_ap_creds.c. A security
// and availability rule that can only be tested by bricking a board is a rule
// nobody tests.
//
// Lengths are OCTETS, not characters: the 802.11 SSID field is a 32-byte
// octet string and a PSK is at most 63 octets. A 20-character name in accented
// Latin-1 is fine as UTF-8 (20 octets); the same name in Japanese is not (60
// octets), and strlen() - which is what counts here - gets that right for free.
//
// An OPEN network (empty password) is deliberately NOT allowed. Every mutating
// command on this board is behind the admin PIN, but an open AP means anyone in
// radio range can read the PIN hash-free plaintext from NVS over the dashboard's
// own console. The escape hatch for "I forgot it" is `reset_ap`, not a blank
// password.

#define AP_MAX_SSID_LEN 32
#define AP_MIN_PASS_LEN 8
#define AP_MAX_PASS_LEN 63

// Each validator returns true/false and, on false, points *reason (when non-null)
// at a static, human-readable sentence WITHOUT leading whitespace. The caller
// owns the formatting, so the same string reads correctly in a console line and
// in a dashboard toast.
//
// *reason is never set on success, so a caller may pass NULL if it does not care
// why something failed.
bool ap_creds_validateSsid(const char *ssid, const char **reason);
bool ap_creds_validatePass(const char *pass, const char **reason);

// Both fields, SSID first. Returns the first failure's reason, so a user who got
// both wrong is told about the network name before the password.
bool ap_creds_validate(const char *ssid, const char *pass, const char **reason);
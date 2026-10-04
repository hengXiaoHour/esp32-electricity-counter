#pragma once
// Cloud (remote-monitoring) config rules, Arduino-free so the host unit test
// scripts/test_cloud_cfg.c can include this directly.
//
// What this is: the board pushes a small JSON snapshot to a Firebase Realtime
// Database over plain HTTPS REST (PATCH), every 10 s, STA-only. There is no
// Firebase SDK on the board - the old SDK's blocking TLS handshake was one of
// the reasons the cloud was ripped out (doc/opencode_agent/lessons.md,
// "Archived - cloud era").
//
// Identity: the RTDB path is /devices/<MAC>/latest where <MAC> is the radio's
// own 12-hex-digit MAC (AA:BB:CC:DD:EE:FF -> AABBCCDDEEFF). There is deliberately
// NO settable board id: the old hand-typed id was a typo-driven orphan factory
// (two boards, one id, or one board re-typed after a reset = a dead node nobody
// reads). The silicon already has a globally unique id; typing another one on
// top only adds a way to be wrong.
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

#define CLOUD_MAX_HOST_LEN 128
#define CLOUD_MAX_AUTH_LEN 256
// 12 hex digits, no separators, upper case. NOT NUL-terminated by contract:
// callers size the buffer CLOUD_DEVICE_ID_LEN+1 and terminate it.
#define CLOUD_DEVICE_ID_LEN 12

// Database host, e.g. my-project-default-rtdb.asia-southeast1.firebasedatabase.app
// or my-project.firebaseio.com. Host ONLY: no https://, no path, no query.
// Rejects empty, over-long, control/whitespace characters, embedded protocol
// ("://") or path ("/"), and dot-less names (a bare "localhost" or typo).
bool cloud_validateHost(const char *host);

// Database secret / auth token pasted from the Firebase console. The board can
// only check shape, not validity: non-empty, bounded, no whitespace or control
// bytes (a pasted token with a trailing newline is the classic failure and must
// be refused at input, not debugged over serial later).
bool cloud_validateAuth(const char *auth);

// "AA:BB:CC:DD:EE:FF" (any case) -> "AABBCCDDEEFF". Returns false unless the
// input is exactly 17 chars of hex pairs separated by colons; out needs
// CLOUD_DEVICE_ID_LEN+1 bytes (NUL-terminated on success, untouched on failure).
bool cloud_formatDeviceId(const char *mac, char *out);

#ifdef __cplusplus
}
#endif

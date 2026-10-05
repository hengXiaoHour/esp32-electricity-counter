#pragma once
// Cloud (remote-monitoring) config rules, Arduino-free so the host unit test
// scripts/test_cloud_cfg.c can include this directly.
//
// What this is: the board pushes a small JSON snapshot to a Firebase Realtime
// Database over plain HTTPS REST (PATCH) on one keep-alive TLS session,
// every 5 s, STA-only. There is no
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
#define CLOUD_MAX_EMAIL_LEN 128
#define CLOUD_MAX_PASS_LEN 128
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

// Herd auth (email/password login): the account the board signs in as.
// Email is loosely shaped (exactly one '@', a dot after it, no spaces -
// Firebase itself is the real validator); the password rule is only
// non-empty and bounded, because Firebase passwords may contain spaces and
// strength was already enforced when the user was created.
bool cloud_validateEmail(const char *email);
bool cloud_validatePass(const char *pass);

// Splits `setcloud <host> <email> <password...>` into three. Host and email
// never contain spaces so they split on whitespace; the password is the
// REMAINDER of the line (leading separator + trailing whitespace trimmed),
// so a password with spaces survives. Returns false on missing parts or any
// overflow (refused, never truncated - same rule as ap_creds_splitArgs).
// Buffers need CLOUD_MAX_HOST_LEN+1 / CLOUD_MAX_EMAIL_LEN+1 /
// CLOUD_MAX_PASS_LEN+1 bytes.
bool cloud_splitArgs3(const char *args, char *host, size_t hostLen,
                      char *email, size_t emailLen, char *pass, size_t passLen);

// base64url decoding (JWT segments: A–Z a–z 0–9 - _ , no padding) for the
// `cloud diag` command, which proves WHAT token the board holds (length +
// audience claim) without ever printing the token. Returns decoded length,
// or -1 on bad input/overflow. out needs ~4/3 of the input length.
int cloud_b64urlDecode(const char *in, char *out, size_t outLen);

// Extracts the "aud" (audience = project id) claim from a JWT's payload
// WITHOUT verifying anything: this is a diagnostic readout, not auth.
// Returns false unless the token has three dot-separated segments and the
// payload contains a string "aud". out needs 64+ bytes for real audiences.
bool cloud_jwtAud(const char *jwt, char *out, size_t outLen);

// "AA:BB:CC:DD:EE:FF" (any case) -> "AABBCCDDEEFF". Returns false unless the
// input is exactly 17 chars of hex pairs separated by colons; out needs
// CLOUD_DEVICE_ID_LEN+1 bytes (NUL-terminated on success, untouched on failure).
bool cloud_formatDeviceId(const char *mac, char *out);

#ifdef __cplusplus
}
#endif

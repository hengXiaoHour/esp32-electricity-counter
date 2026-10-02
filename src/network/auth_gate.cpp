#include "auth_gate.h"

#include <string.h>

static const char *const PIN_EXEMPT_VERBS[] = {"set_time", "verify_pin", NULL};

bool auth_isExemptVerb(const char *verb) {
  if (verb == NULL || verb[0] == '\0') return false;
  for (const char *const *v = PIN_EXEMPT_VERBS; *v != NULL; v++) {
    if (strcmp(*v, verb) == 0) return true;
  }
  return false;
}

// Copies the verb value out of a "cmd":"..." field.
static bool extractVerb(const char *frame, char *out, size_t outLen) {
  if (frame == NULL || out == NULL || outLen == 0) return false;
  const char *needle = "\"cmd\":\"";
  const char *p = strstr(frame, needle);
  if (p == NULL) return false;
  p += strlen(needle);
  const char *end = strchr(p, '"');
  if (end == NULL || end == p) return false;
  size_t n = (size_t)(end - p);
  if (n >= outLen) n = outLen - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

bool auth_extractPin(const char *frame, char *out, size_t outLen) {
  if (frame == NULL || out == NULL || outLen == 0) return false;
  out[0] = '\0';

  const char *needle = "\"pin\":\"";
  const char *p = strstr(frame, needle);
  if (p == NULL) return false;
  p += strlen(needle);

  size_t n = 0;
  while (*p != '\0') {
    if (*p == '\\' && p[1] != '\0') {
      // JSON escapes that can legitimately appear in a PIN.
      char c = p[1];
      p += 2;
      switch (c) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        default:  break;  // \" \\ \/ and anything else pass through
      }
      if (n + 1 >= outLen) break;
      out[n++] = c;
      continue;
    }
    if (*p == '"') {
      out[n] = '\0';
      return true;
    }
    if (n + 1 >= outLen) {
      // Buffer full before the field closed: treat as malformed rather than
      // silently truncating a PIN (which could make two different PINs compare
      // equal after truncation).
      out[0] = '\0';
      return false;
    }
    out[n++] = *p++;
  }
  // Fell out of the loop: end of input or buffer exhaustion with no closing
  // quote. Either way the frame is truncated. Earlier this returned true with
  // whatever it had read, so a frame like {"cmd":"set_name","pin":"1234  (no
  // closing quote) authenticated as 1234. Malformed input must never
  // authenticate.
  out[0] = '\0';
  return false;
}

bool auth_check(const char *frame, const char *expectedPin, char *outVerb,
                size_t outVerbLen) {
  if (outVerb != NULL && outVerbLen > 0) outVerb[0] = '\0';

  // A frame with no recognisable verb is not this gate's business — the caller
  // will reject it as an unknown command. Treating it as exempt here keeps the
  // gate from claiming to have authenticated something it never parsed.
  char verb[AUTH_MAX_VERB];
  if (!extractVerb(frame, verb, sizeof(verb))) return true;
  if (outVerb != NULL) {
    strncpy(outVerb, verb, outVerbLen - 1);
    outVerb[outVerbLen - 1] = '\0';
  }

  if (auth_isExemptVerb(verb)) return true;

  char got[AUTH_MAX_PIN];
  if (!auth_extractPin(frame, got, sizeof(got))) return false;
  if (got[0] == '\0') return false;
  if (expectedPin == NULL || expectedPin[0] == '\0') return false;

  // Constant-time-ish compare. Not cryptographic and does not need to be: the
  // attacker is on the same AP and can time a thousand guesses either way. It
  // does avoid leaking the prefix length through an early return.
  size_t la = strlen(got), lb = strlen(expectedPin);
  unsigned char diff = (unsigned char)(la ^ lb);
  size_t n = la < lb ? la : lb;
  for (size_t i = 0; i < n; i++) {
    diff |= (unsigned char)(got[i] ^ expectedPin[i]);
  }
  return diff == 0;
}
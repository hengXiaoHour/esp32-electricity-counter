#include "ap_creds.h"

#include <string.h>

// Shared by both validators: a C0 control byte anywhere in a credential.
// Allowed for neither, because neither can be typed into a phone's WiFi dialog
// and a stored value the user cannot reproduce is a stored value that locks them
// out. (Bytes >= 0x80 are untouched: those are UTF-8 continuation bytes, and
// non-ASCII names are legitimate.)
static bool has_control_byte(const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p < 0x20) return true;
  }
  return false;
}

bool ap_creds_validateSsid(const char *ssid, const char **reason) {
  const char *why = NULL;

  if (ssid == NULL) {
    why = "Network name is missing";
  } else {
    size_t len = strlen(ssid);
    if (len == 0) {
      why = "Network name cannot be empty";
    } else if (len > AP_MAX_SSID_LEN) {
      why = "Network name must be 32 characters or fewer";
    } else if (has_control_byte(ssid)) {
      why = "Network name cannot contain control characters";
    } else if (ssid[0] == ' ' || ssid[len - 1] == ' ') {
      // Not cosmetic. A trailing space is invisible on a phone, so the board
      // broadcasts a name nobody can type and the user concludes the board is
      // broken. Rejecting beats silently trimming and storing something other
      // than what they typed.
      why = "Network name cannot start or end with a space";
    } else {
      bool all_spaces = true;
      for (const char *p = ssid; *p; p++) {
        if (*p != ' ') { all_spaces = false; break; }
      }
      if (all_spaces) why = "Network name cannot be only spaces";
    }
  }

  if (why && reason) *reason = why;
  return why == NULL;
}

bool ap_creds_validatePass(const char *pass, const char **reason) {
  const char *why = NULL;

  if (pass == NULL) {
    why = "Password is missing";
  } else {
    size_t len = strlen(pass);
    if (len == 0) {
      // An open AP is not offered: see the note in ap_creds.h.
      why = "Password cannot be empty (an open network is not allowed)";
    } else if (len < AP_MIN_PASS_LEN) {
      why = "Password must be at least 8 characters";
    } else if (len > AP_MAX_PASS_LEN) {
      why = "Password must be 63 characters or fewer";
    } else if (has_control_byte(pass)) {
      why = "Password cannot contain control characters";
    }
  }

  if (why && reason) *reason = why;
  return why == NULL;
}

bool ap_creds_validate(const char *ssid, const char *pass, const char **reason) {
  if (!ap_creds_validateSsid(ssid, reason)) return false;
  return ap_creds_validatePass(pass, reason);
}

// Copies a possibly-quoted field out of `*p`, advancing `*p` past it.
// Returns false when a quote is opened and never closed, OR when the field does
// not fit the destination.
//
// It refuses rather than truncating. Silently shortening an over-long name would
// produce a *valid* 32-character SSID out of a 39-character one, and the board
// would then broadcast something other than what was typed - a rename that
// appears to work and is wrong. Refusing costs one message and keeps the promise
// that what is stored is what was asked for.
static bool takeField(const char **p, char *out, size_t outLen) {
  const char *s = *p;
  size_t n;
  if (*s == '"') {
    const char *close = strchr(s + 1, '"');
    if (close == NULL) return false;      // opened, never closed
    n = (size_t)(close - (s + 1));
    if (n >= outLen) return false;        // would overflow
    memcpy(out, s + 1, n);
    out[n] = '\0';
    *p = close + 1;
  } else {
    const char *end = strchr(s, ' ');
    n = (end != NULL) ? (size_t)(end - s) : strlen(s);
    if (n >= outLen) return false;        // would overflow
    memcpy(out, s, n);
    out[n] = '\0';
    *p = (end != NULL) ? end : s + strlen(s);
  }
  return true;
}

bool ap_creds_splitArgs(const char *args, char *ssid, size_t ssidLen,
                        char *pass, size_t passLen) {
  if (ssid == NULL || pass == NULL || ssidLen == 0 || passLen == 0) return false;
  ssid[0] = '\0';
  pass[0] = '\0';
  if (args == NULL) return false;

  // Leading whitespace: the console trims the line, but the web console sends
  // `line` verbatim and a leading space there would otherwise parse as an
  // empty name.
  const char *p = args;
  while (*p == ' ' || *p == '\t') p++;

  if (*p == '"') {
    const char *close = strchr(p + 1, '"');
    if (close == NULL) return false;
    size_t n = (size_t)(close - (p + 1));
    if (n >= ssidLen) return false;        // would overflow
    memcpy(ssid, p + 1, n);
    ssid[n] = '\0';
    p = close + 1;
  } else {
    const char *end = strchr(p, ' ');
    if (end == NULL || end == p) return false;   // no separator / empty name
    size_t n = (size_t)(end - p);
    if (n >= ssidLen) return false;        // would overflow
    memcpy(ssid, p, n);
    ssid[n] = '\0';
    p = end;
  }

  while (*p == ' ' || *p == '\t') p++;
  if (*p == '\0') return false;                 // a name with no password
  if (!takeField(&p, pass, passLen)) return false;
  if (*p == '\0') return true;

  // Anything left over is refused rather than silently ignored. `set_ap Meter
  // hunter2 oops` is almost certainly a typo, and storing "hunter2 oops" would
  // look like it worked right up until the phone could not join. Trailing
  // whitespace is the one exception: the console trims the line, so those
  // spaces are not part of anything.
  while (*p == ' ' || *p == '\t') p++;
  return *p == '\0';
}
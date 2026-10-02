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
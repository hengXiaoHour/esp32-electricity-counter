#include "cloud_cfg.h"

static bool isHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static char upHex(char c) {
  if (c >= 'a' && c <= 'f') return (char)(c - 'a' + 'A');
  return c;
}

bool cloud_validateHost(const char *host) {
  if (!host) return false;
  size_t n = 0;
  while (host[n] != '\0') n++;
  if (n == 0 || n > CLOUD_MAX_HOST_LEN) return false;
  bool dot = false;
  for (size_t i = 0; i < n; i++) {
    char c = host[i];
    if (c <= 0x20 || c == 0x7f) return false;  // whitespace / control / DEL
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-';
    if (!ok) return false;  // covers ':' (protocol/paste of a URL) and '/' (path)
    if (c == '.') dot = true;
  }
  if (!dot) return false;
  if (host[0] == '.' || host[0] == '-') return false;
  if (host[n - 1] == '.' || host[n - 1] == '-') return false;
  return true;
}

bool cloud_validateAuth(const char *auth) {
  if (!auth) return false;
  size_t n = 0;
  while (auth[n] != '\0') n++;
  if (n == 0 || n > CLOUD_MAX_AUTH_LEN) return false;
  for (size_t i = 0; i < n; i++) {
    // Tokens are printable with no spaces; a trailing newline from a console
    // paste is the failure this exists to catch.
    if (auth[i] <= 0x20 || auth[i] == 0x7f) return false;
  }
  return true;
}

bool cloud_formatDeviceId(const char *mac, char *out) {
  if (!mac || !out) return false;
  // Exactly "XX:XX:XX:XX:XX:XX" - 17 chars, colons at 2,5,8,11,14.
  for (int i = 0; i < 17; i++) {
    char c = mac[i];
    if (c == '\0') return false;
    if (i % 3 == 2) {
      if (c != ':') return false;
    } else {
      if (!isHex(c)) return false;
    }
  }
  if (mac[17] != '\0') return false;
  int o = 0;
  for (int i = 0; i < 17; i++) {
    if (mac[i] == ':') continue;
    out[o++] = upHex(mac[i]);
  }
  out[o] = '\0';
  return o == CLOUD_DEVICE_ID_LEN;
}

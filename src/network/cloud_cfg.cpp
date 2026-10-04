#include "cloud_cfg.h"

#include <string.h>

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

bool cloud_validateEmail(const char *email) {
  if (!email) return false;
  size_t n = 0;
  while (email[n] != '\0') n++;
  if (n == 0 || n > CLOUD_MAX_EMAIL_LEN) return false;
  int at = -1;
  for (size_t i = 0; i < n; i++) {
    char c = email[i];
    if (c <= 0x20 || c == 0x7f) return false;
    if (c == '@') {
      if (at >= 0) return false;  // exactly one '@'
      at = (int)i;
    }
  }
  if (at <= 0 || at >= (int)n - 1) return false;  // something on both sides
  // A dot somewhere after the '@' (loose - Firebase decides for real).
  for (size_t i = (size_t)at + 1; i < n; i++) {
    if (email[i] == '.') return true;
  }
  return false;
}

bool cloud_validatePass(const char *pass) {
  if (!pass) return false;
  size_t n = 0;
  while (pass[n] != '\0') n++;
  if (n == 0 || n > CLOUD_MAX_PASS_LEN) return false;
  // Spaces ALLOWED (Firebase permits them); control bytes are not.
  for (size_t i = 0; i < n; i++) {
    if (pass[i] < 0x20 || pass[i] == 0x7f) return false;
  }
  return true;
}

bool cloud_splitArgs3(const char *args, char *host, size_t hostLen,
                      char *email, size_t emailLen, char *pass, size_t passLen) {
  if (!args || !host || !email || !pass ||
      hostLen == 0 || emailLen == 0 || passLen == 0) return false;
  host[0] = '\0';
  email[0] = '\0';
  pass[0] = '\0';

  const char *p = args;
  while (*p == ' ' || *p == '\t') p++;

  // Token 1: host (to whitespace).
  const char *end = p;
  while (*end != '\0' && *end != ' ' && *end != '\t') end++;
  if (end == p) return false;
  if ((size_t)(end - p) >= hostLen) return false;  // refused, never truncated
  memcpy(host, p, (size_t)(end - p));
  host[end - p] = '\0';
  p = end;
  while (*p == ' ' || *p == '\t') p++;

  // Token 2: email (to whitespace).
  end = p;
  while (*end != '\0' && *end != ' ' && *end != '\t') end++;
  if (end == p) return false;
  if ((size_t)(end - p) >= emailLen) return false;
  memcpy(email, p, (size_t)(end - p));
  email[end - p] = '\0';
  p = end;
  while (*p == ' ' || *p == '\t') p++;

  // Remainder: password (may contain spaces). Trim trailing whitespace only -
  // the console already strips the newline, but a pasted line may trail.
  if (*p == '\0') return false;
  const char *tail = p + strlen(p);
  while (tail > p && (tail[-1] == ' ' || tail[-1] == '\t')) tail--;
  if (tail == p) return false;
  if ((size_t)(tail - p) >= passLen) return false;
  memcpy(pass, p, (size_t)(tail - p));
  pass[tail - p] = '\0';
  return true;
}

static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

int cloud_b64urlDecode(const char *in, char *out, size_t outLen) {
  if (!in || !out || outLen == 0) return -1;
  size_t n = strlen(in);
  if (n == 0 || n % 4 == 1) return -1;  // padding-less lengths are never 1 mod 4
  size_t o = 0;
  for (size_t i = 0; i < n;) {
    int v[4] = {0, 0, 0, 0};
    int k = 0;
    for (; k < 4 && i < n; k++, i++) {
      int b = b64val(in[i]);
      if (b < 0) return -1;
      v[k] = b;
    }
    // Short final quantum: missing chars read as zero (padding omitted).
    if (o + 3 > outLen) return -1;
    out[o++] = (char)((v[0] << 2) | (v[1] >> 4));
    if (k > 2) out[o++] = (char)(((v[1] & 0x0f) << 4) | (v[2] >> 2));
    if (k > 3) out[o++] = (char)(((v[2] & 0x03) << 6) | v[3]);
  }
  if (o >= outLen) return -1;
  out[o] = '\0';
  return (int)o;
}

bool cloud_jwtAud(const char *jwt, char *out, size_t outLen) {
  if (!jwt || !out || outLen == 0) return false;
  const char *d1 = strchr(jwt, '.');
  if (!d1) return false;
  const char *d2 = strchr(d1 + 1, '.');
  if (!d2) return false;
  size_t segLen = (size_t)(d2 - (d1 + 1));
  if (segLen == 0 || segLen >= 1024) return false;
  static char seg[1024];
  static char payload[1400];
  memcpy(seg, d1 + 1, segLen);
  seg[segLen] = '\0';
  if (cloud_b64urlDecode(seg, payload, sizeof(payload)) < 0) return false;
  // String-valued "aud" only. (Array-form aud exists in the wild but Google
  // ID tokens use the string form; refusing the exotic shape here is safer
  // than guessing which element to print.)
  const char *key = "\"aud\":\"";
  const char *p = strstr(payload, key);
  if (!p) return false;
  p += strlen(key);
  const char *q = strchr(p, '"');
  if (!q || q == p || (size_t)(q - p) >= outLen) return false;
  memcpy(out, p, (size_t)(q - p));
  out[q - p] = '\0';
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

// Host test for the JSON string escaper used by system_json.cpp.
//
//   gcc -std=c11 -Wall -Wextra -x c scripts/test_json_escape.c
//       -o /tmp/opencode/json_escape_test
//
// Why this exists: ap_creds_validateSsid accepts `"` and `\` in a network name
// (they are legal for an 802.11 SSID and perfectly typeable on a phone). The
// board's system JSON is rebuilt and pushed ~7 times a second, so one unescaped
// quote turns every broadcast into a frame JSON.parse() throws away - taking the
// dashboard's live readings with it, not just the Access Point panel.
//
// The function under test lives in system_json.cpp, which needs Arduino.h. This
// file re-declares the same loop over a minimal String stand-in, so the logic is
// verified without dragging the Arduino core onto the host. The copy is
// verbatim; check_docs.py asserts the two stay in step.

#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* --- minimal String stand-in (Arduino.h is not available on the host) --- */
typedef struct { char buf[256]; size_t len; } MiniString;

static void ms_init(MiniString *s) { s->buf[0] = '\0'; s->len = 0; }
static void ms_append(MiniString *s, char c) { s->buf[s->len++] = c; s->buf[s->len] = '\0'; }

/* --- verbatim copy of appendJsonEscaped() from src/network/system_json.cpp --- */
static void appendJsonEscaped(MiniString *json, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      ms_append(json, '\\');
      ms_append(json, c);
    } else if ((unsigned char)c < 0x20) {
      ms_append(json, ' ');
    } else {
      ms_append(json, c);
    }
  }
}

static int failures = 0;
static int checks = 0;

static void check(const char *name, int ok, const char *detail) {
  checks++;
  if (ok) {
    printf("  ok   %s\n", name);
  } else {
    failures++;
    printf("  FAIL %s  -> %s\n", name, detail ? detail : "");
  }
}

static void esc(const char *in, char *out, size_t outLen) {
  MiniString j; ms_init(&j);
  appendJsonEscaped(&j, in, strlen(in));
  snprintf(out, outLen, "%s", j.buf);
}

/* A minimal strict-ish JSON string reader: enough to prove the body parses.
 * Walks from the opening quote and returns 1 on a well-formed close. */
static int bodyIsWellFormed(const char *body) {
  int i = 0;
  if (body[i++] != '"') return 0;
  while (body[i]) {
    if (body[i] == '\\') {
      char n = body[i + 1];
      if (n != '"' && n != '\\') return 0;
      i += 2;
    } else if (body[i] == '"') {
      return body[i + 1] == '\0';   /* close must be the end */
    } else if ((unsigned char)body[i] < 0x20) {
      return 0;                    /* raw control byte: invalid JSON */
    } else {
      i++;
    }
  }
  return 0;                        /* never closed */
}

int main(void) {
  char out[512];

  /* --- the ordinary cases must pass through untouched --- */
  esc("Lab Meter", out, sizeof out);
  check("a plain name is unchanged", strcmp(out, "Lab Meter") == 0, out);

  esc("", out, sizeof out);
  check("an empty name stays empty", strcmp(out, "") == 0, out);

  esc("Ben \xe2\x80\x99s Meter", out, sizeof out);
  check("UTF-8 passes through byte for byte",
        strcmp(out, "Ben \xe2\x80\x99s Meter") == 0, out);

  /* --- the case that actually breaks a naive serialiser --- */
  esc("Ben \"the meter\" Lab", out, sizeof out);
  check("an embedded double quote is escaped",
        strcmp(out, "Ben \\\"the meter\\\" Lab") == 0, out);

  esc("back\\slash", out, sizeof out);
  check("an embedded backslash is escaped",
        strcmp(out, "back\\\\slash") == 0, out);

  esc("mix \"a\\b\" end", out, sizeof out);
  check("both together are escaped",
        strcmp(out, "mix \\\"a\\\\b\\\" end") == 0, out);

  /* --- every case must still produce a parseable JSON string body --- */
  const char *names[] = {
    "Lab Meter",
    "Ben \"the meter\" Lab",
    "back\\slash",
    "mix \"a\\b\" end",
    "\"",
    "\\",
    "\"\\\"",
    "tab\there",
    "ESP32-Elec-Counter",
    NULL
  };
  for (int i = 0; names[i]; i++) {
    char w[512];
    esc(names[i], w, sizeof w);
    char wrapped[520];
    snprintf(wrapped, sizeof wrapped, "\"%s\"", w);
    char detail[600];
    snprintf(detail, sizeof detail, "input=%s -> %s", names[i], wrapped);
    check("the escaped body is a well-formed JSON string", bodyIsWellFormed(wrapped), detail);
  }

  /* --- negative control: the checker must reject what it claims to --- */
  check("NEGATIVE CONTROL: an unescaped quote is rejected",
        !bodyIsWellFormed("\"Lab \"Meter\""),
        "the naive output passed its own checker");
  check("NEGATIVE CONTROL: a raw control byte is rejected",
        !bodyIsWellFormed("\"Lab\tMeter\""),
        "the naive output passed its own checker");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
/* Host-side unit tests for the admin-PIN gate.
 *
 * The gate is the security boundary of an AP-only board: anyone in radio range
 * can open the dashboard. This exercises it without hardware.
 *
 * Build + run:
 *   gcc -std=c11 -Wall -Wextra -Isrc/network \
 *       scripts/test_auth_gate.c src/network/auth_gate.cpp -o /tmp/auth_gate_test
 *   /tmp/auth_gate_test
 *
 * Exits non-zero on the first failure. Every test below asserts a REJECT as
 * loudly as an ACCEPT — a gate test that only proves the happy path would pass
 * against a function that returned true unconditionally.
 */
#include "auth_gate.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void expect(const char *name, bool got, bool want) {
  checks++;
  if (got != want) {
    failures++;
    printf("  FAIL %-58s got %s want %s\n", name, got ? "ALLOW" : "REJECT",
           want ? "ALLOW" : "REJECT");
  } else {
    printf("  ok   %-58s %s\n", name, got ? "ALLOW" : "REJECT");
  }
}

int main(void) {
  const char *PIN = "1234";
  char verb[AUTH_MAX_VERB];

  printf("== exempt verbs ==\n");
  expect("set_time is exempt",
         auth_check("{\"cmd\":\"set_time\",\"t\":1759400000}", PIN, verb, sizeof(verb)),
         true);
  expect("verify_pin is exempt",
         auth_check("{\"cmd\":\"verify_pin\"}", PIN, verb, sizeof(verb)), true);
  expect("exempt verb reported to caller",
         strcmp(auth_check("{\"cmd\":\"set_time\"}", PIN, verb, sizeof(verb)), verb,
                "set_time") == 0 || strcmp(verb, "set_time") == 0,
         true);

  printf("\n== mutating verbs require the PIN ==\n");
  const char *mutating[] = {
      "set_name",     "reset_counter",  "test_inject",   "set_voltage_cal",
      "set_current_cal", "set_monthly_kwh", "set_noise_floor", "set_lpf",
      "set_rms_samples", "set_ntfy_topic", "set_ntfy_enabled", "reset_ch_cal",
      "reset_ch_to_default", "reset_nvs_defaults", "test_force_rollover",
      "console",      "set_pin"};
  for (size_t i = 0; i < sizeof(mutating) / sizeof(mutating[0]); i++) {
    char frame[128];
    snprintf(frame, sizeof(frame), "{\"cmd\":\"%s\",\"ch\":0}", mutating[i]);
    char label[128];
    snprintf(label, sizeof(label), "%s with NO pin is rejected", mutating[i]);
    expect(label, auth_check(frame, PIN, verb, sizeof(verb)), false);
  }

  printf("\n== correct PIN is accepted ==\n");
  expect("set_name + correct pin",
         auth_check("{\"cmd\":\"set_name\",\"ch\":0,\"name\":\"Kitchen\",\"pin\":\"1234\"}",
                    PIN, verb, sizeof(verb)),
         true);
  expect("console + correct pin",
         auth_check("{\"cmd\":\"console\",\"line\":\"status\",\"pin\":\"1234\"}", PIN,
                    verb, sizeof(verb)),
         true);
  expect("set_pin + correct pin",
         auth_check("{\"cmd\":\"set_pin\",\"pin\":\"1234\",\"pin_new\":\"9999\"}", PIN,
                    verb, sizeof(verb)),
         true);

  printf("\n== wrong / malformed PIN is rejected ==\n");
  expect("wrong pin",
         auth_check("{\"cmd\":\"set_name\",\"ch\":0,\"pin\":\"9999\"}", PIN, verb,
                    sizeof(verb)),
         false);
  expect("empty pin string",
         auth_check("{\"cmd\":\"set_name\",\"ch\":0,\"pin\":\"\"}", PIN, verb,
                    sizeof(verb)),
         false);
  expect("prefix of correct pin",
         auth_check("{\"cmd\":\"set_name\",\"pin\":\"123\"}", PIN, verb, sizeof(verb)),
         false);
  expect("correct pin + trailing space",
         auth_check("{\"cmd\":\"set_name\",\"pin\":\"1234 \"}", PIN, verb, sizeof(verb)),
         false);
  expect("correct pin + trailing newline",
         auth_check("{\"cmd\":\"set_name\",\"pin\":\"1234\\n\"}", PIN, verb, sizeof(verb)),
         false);
  expect("device has no pin configured",
         auth_check("{\"cmd\":\"set_name\",\"pin\":\"1234\"}", "", verb, sizeof(verb)),
         false);
  expect("expectedPin is NULL",
         auth_check("{\"cmd\":\"set_name\",\"pin\":\"1234\"}", NULL, verb, sizeof(verb)),
         false);

  printf("\n== pin field smuggling ==\n");
  /* A frame carrying the PIN only inside another field must NOT authenticate.
   * auth_extractPin anchors on the literal "pin":" so a key such as
   * "admin_pin":" cannot satisfy it. */
  expect("PIN hidden in admin_pin field",
         auth_check("{\"cmd\":\"set_name\",\"admin_pin\":\"1234\"}", PIN, verb,
                    sizeof(verb)),
         false);
  expect("PIN hidden in a name value",
         auth_check("{\"cmd\":\"set_name\",\"name\":\"pin\\\":\\\"1234\"}", PIN, verb,
                    sizeof(verb)),
         false);
  expect("unterminated pin field",
         auth_check("{\"cmd\":\"set_name\",\"pin\":\"1234", PIN, verb, sizeof(verb)),
         false);

  printf("\n== verb exemption cannot be smuggled ==\n");
  /* Only the verb decides exemption, and only from the "cmd" field. */
  expect("cmd=verify_pin with wrong pin still allowed (exempt by design)",
         auth_check("{\"cmd\":\"verify_pin\",\"pin\":\"0000\"}", PIN, verb, sizeof(verb)),
         true);
  expect("set_time as a VALUE does not exempt set_name",
         auth_check("{\"cmd\":\"set_name\",\"note\":\"set_time\",\"ch\":0}", PIN, verb,
                    sizeof(verb)),
         false);
  expect("verb 'set_timeX' is not exempt",
         auth_check("{\"cmd\":\"set_timeX\"}", PIN, verb, sizeof(verb)), false);
  expect("verb 'SET_TIME' is not exempt (case-sensitive)",
         auth_check("{\"cmd\":\"SET_TIME\"}", PIN, verb, sizeof(verb)), false);
  expect("empty verb + pin passes through (caller rejects as unknown)",
         auth_check("{\"pin\":\"1234\"}", PIN, verb, sizeof(verb)), true);

  printf("\n== JSON escapes in the PIN ==\n");
  char got[AUTH_MAX_PIN];
  expect("escaped quote decodes",
         auth_extractPin("{\"pin\":\"12\\\"34\"}", got, sizeof(got)) &&
             strcmp(got, "12\"34") == 0,
         true);
  expect("escaped backslash decodes",
         auth_extractPin("{\"pin\":\"12\\\\34\"}", got, sizeof(got)) &&
             strcmp(got, "12\\34") == 0,
         true);
  expect("forward slash decodes",
         auth_extractPin("{\"pin\":\"12/34\"}", got, sizeof(got)) &&
             strcmp(got, "12/34") == 0,
         true);
  expect("escaped pin matches expected pin",
         auth_check("{\"cmd\":\"set_name\",\"pin\":\"12\\\"34\"}", "12\"34", verb,
                    sizeof(verb)),
         true);

  printf("\n== PIN values containing quotes (round trip) ==\n");
  expect("pin with escaped newline is rejected unless expected matches",
         auth_check("{\"cmd\":\"set_name\",\"pin\":\"12\\n34\"}", "1234", verb,
                    sizeof(verb)),
         false);

  printf("\n== null / empty input ==\n");
  expect("NULL frame",
         auth_check(NULL, PIN, verb, sizeof(verb)), true);
  expect("empty frame",
         auth_check("", PIN, verb, sizeof(verb)), true);
  expect("empty outVerbLen does not crash",
         auth_check("{\"cmd\":\"set_name\",\"pin\":\"1234\"}", PIN, verb, 0), true);

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
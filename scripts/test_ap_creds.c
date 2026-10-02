/* Host-side unit tests for the soft-AP credential rules.
 *
 * Build + run:
 *   gcc -std=c11 -Wall -Wextra -Isrc/network \
 *       scripts/test_ap_creds.c src/network/ap_creds.cpp -o /tmp/ap_creds_test
 *   /tmp/ap_creds_test
 *
 * Exits non-zero if any check failed.
 *
 * Two things this file deliberately does that a naive version would not:
 *
 *  1. It asserts the BOUNDARIES (7/8/63/64 and 1/32/33 characters), not just a
 *     short and a long value. The off-by-one that matters here is the one that
 *     would let a 7-character password through and take the radio down with it.
 *
 *  2. It runs a NEGATIVE CONTROL on its own assertion helper before trusting any
 *     PASS: expect("self-check", false, true) must actually increment the failure
 *     count. A test harness that cannot fail has not tested anything, and this
 *     repo has been bitten by exactly that before (scripts/check_deadcode.py rule
 *     4, which could never fire).
 */
#include "ap_creds.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void expect(const char *name, bool got, bool want) {
  checks++;
  if (got != want) {
    failures++;
    printf("  FAIL %-56s got %s want %s\n", name, got ? "ACCEPT" : "REJECT",
           want ? "ACCEPT" : "REJECT");
  } else {
    printf("  ok   %-56s %s\n", name, got ? "ACCEPT" : "REJECT");
  }
}

static void expectReason(const char *name, const char *got, const char *want) {
  checks++;
  if (want == NULL) {
    if (got != NULL) {
      failures++;
      printf("  FAIL %-56s reason should be NULL, got \"%s\"\n", name, got);
    } else {
      printf("  ok   %-56s reason NULL\n", name);
    }
    return;
  }
  if (got == NULL || strcmp(got, want) != 0) {
    failures++;
    printf("  FAIL %-56s reason \"%s\" want \"%s\"\n", name,
           got ? got : "(null)", want);
  } else {
    printf("  ok   %-56s reason ok\n", name);
  }
}

/* Convenience: validate a pair and hand back the reason (NULL on success). */
static const char *why(const char *ssid, const char *pass) {
  const char *r = NULL;
  ap_creds_validate(ssid, pass, &r);
  return r;
}

int main(void) {
  printf("== the harness can actually fail (negative control) ==\n");
  {
    int before = failures;
    expect("self-check: a wrong expectation must be reported", false, true);
    if (failures != before + 1) {
      printf("  FAIL the assertion helper did not record a failure\n");
      return 1;
    }
    printf("  ok   helper recorded the failure; resetting for the real run\n");
    failures = 0;
  }
  {
    int before = failures;
    expectReason("self-check: wrong expected reason must be reported", "actual",
                 "different");
    if (failures != before + 1) {
      printf("  FAIL the reason helper did not record a failure\n");
      return 1;
    }
    failures = 0;
  }

  printf("\n== SSID ==\n");
  expect("plain name is accepted", ap_creds_validateSsid("Meter", NULL), true);
  expect("name with spaces inside is accepted",
         ap_creds_validateSsid("Living Room Meter", NULL), true);
  expect("name with punctuation is accepted",
         ap_creds_validateSsid("Chen's meter #2 (main)", NULL), true);
  expect("non-ASCII name is accepted",
         ap_creds_validateSsid("Kraftmesser-Ü", NULL), true);

  {
    /* 32 bytes: the 802.11 field size exactly. */
    char max[AP_MAX_SSID_LEN + 1];
    memset(max, 'A', AP_MAX_SSID_LEN);
    max[AP_MAX_SSID_LEN] = '\0';
    expect("32 bytes is accepted", ap_creds_validateSsid(max, NULL), true);

    /* 33 bytes: one over. */
    char over[AP_MAX_SSID_LEN + 2];
    memset(over, 'A', AP_MAX_SSID_LEN + 1);
    over[AP_MAX_SSID_LEN + 1] = '\0';
    expect("33 bytes is rejected", ap_creds_validateSsid(over, NULL), false);
  }

  expect("empty name is rejected", ap_creds_validateSsid("", NULL), false);
  expect("NULL name is rejected", ap_creds_validateSsid(NULL, NULL), false);
  expect("name of only spaces is rejected", ap_creds_validateSsid("   ", NULL), false);
  expect("leading space is rejected", ap_creds_validateSsid(" Meter", NULL), false);
  expect("trailing space is rejected", ap_creds_validateSsid("Meter ", NULL), false);
  expect("embedded newline is rejected", ap_creds_validateSsid("Me\nter", NULL), false);
  expect("embedded tab is rejected", ap_creds_validateSsid("Me\tter", NULL), false);
  /* Length is counted in OCTETS, not characters. 16 three-byte characters is
   * 48 bytes and must be rejected even though "16 characters" is short. */
  const char *wide16 =
      "\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80"
      "\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80"
      "\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80"
      "\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80";
  const char *wide10 =
      "\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80"
      "\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80\xe9\xb8\x80"
      "\xe9\xb8\x80\xe9\xb8\x80";
  /* Assert the fixtures are what the labels claim. The first version of this
   * test wrote 12 characters into the "10 characters" case, so the validator was
   * right and the test was wrong - and the only symptom was a red line that
   * invited "fix the code to make the test pass". */
  expectReason("the 16-character fixture really is 48 bytes",
               strlen(wide16) == 48 ? NULL : "fixture is not 48 bytes", NULL);
  expectReason("the 10-character fixture really is 30 bytes",
               strlen(wide10) == 30 ? NULL : "fixture is not 30 bytes", NULL);
  expect("16 three-byte characters (48 bytes) is rejected",
         ap_creds_validateSsid(wide16, NULL), false);
  /* ...and 10 of them (30 bytes) must be ACCEPTED, which is what proves the
   * check is counting bytes and not characters, and not rejecting all non-ASCII. */
  expect("10 three-byte characters (30 bytes) is accepted",
         ap_creds_validateSsid(wide10, NULL), true);

  printf("\n== password ==\n");
  expect("8 characters (the WPA2 minimum) is accepted",
         ap_creds_validatePass("12345678", NULL), true);
  expect("63 characters (the WPA2 maximum) is accepted",
         ap_creds_validatePass("123456789012345678901234567890123456789012345678901234567890123", NULL),
         true);
  expect("7 characters is rejected", ap_creds_validatePass("1234567", NULL), false);
  expect("64 characters is rejected",
         ap_creds_validatePass("1234567890123456789012345678901234567890123456789012345678901234", NULL),
         false);
  expect("empty password (open AP) is rejected", ap_creds_validatePass("", NULL), false);
  expect("NULL password is rejected", ap_creds_validatePass(NULL, NULL), false);
  expect("password with punctuation is accepted",
         ap_creds_validatePass("p@ss w0rd!#%&*", NULL), true);
  expect("password with a quote is accepted",
         ap_creds_validatePass("he said \"hi\"", NULL), true);
  expect("password with a backslash is accepted",
         ap_creds_validatePass("back\\slash", NULL), true);
  expect("password with a newline is rejected",
         ap_creds_validatePass("abc\ndefgh", NULL), false);

  printf("\n== the pair validator reports the FIRST problem ==\n");
  expect("valid pair passes", ap_creds_validate("Meter", "12345678", NULL), true);
  expect("bad name + bad password still fails", ap_creds_validate("", "", NULL), false);
  /* An empty password is the mistake that actually bricks the radio, so when
   * BOTH fields are wrong the user must be told about the password rule only
   * after the name rule passes - i.e. the name is reported first. */
  expectReason("bad name is reported before a bad password",
               why("Meter ", "short"), "Network name cannot start or end with a space");
  expectReason("a bad password is reported once the name is fine",
               why("Meter", "short"), "Password must be at least 8 characters");
  expectReason("empty password explains the open-network rule",
               why("Meter", ""),
               "Password cannot be empty (an open network is not allowed)");
  expectReason("a valid pair leaves the reason NULL", why("Meter", "12345678"), NULL);

  printf("\n== *reason may be NULL on failure ==\n");
  /* The API promises a NULL-tolerant caller; if the implementation wrote through
   * a NULL pointer this would segfault rather than report. */
  expect("validate with a NULL reason out-param survives failure",
         ap_creds_validate("Meter", "short", NULL), false);
  expect("validateSsid with a NULL reason out-param survives failure",
         ap_creds_validateSsid("", NULL), false);

  printf("\n== serial console argument splitting ==\n");
  {
    char ssid[AP_MAX_SSID_LEN + 1];
    char pass[AP_MAX_PASS_LEN + 1];

    expect("plain two fields", ap_creds_splitArgs("Meter hunter2hunter2", ssid,
                                                  sizeof(ssid), pass, sizeof(pass)), true);
    expectReason("...name parsed", ssid, "Meter");
    expectReason("...password parsed", pass, "hunter2hunter2");

    expect("quoted name with spaces",
           ap_creds_splitArgs("\"Living Room Meter\" hunter2hunter2", ssid,
                              sizeof(ssid), pass, sizeof(pass)), true);
    expectReason("...spaces preserved inside the quotes", ssid, "Living Room Meter");

    expect("quoted password", ap_creds_splitArgs("Meter \"pass word\"", ssid,
                                                 sizeof(ssid), pass, sizeof(pass)), true);
    expectReason("...inner space kept", pass, "pass word");

    expect("both quoted",
           ap_creds_splitArgs("\"My Meter\" \"my pass\"", ssid, sizeof(ssid),
                              pass, sizeof(pass)), true);
    expectReason("...name", ssid, "My Meter");
    expectReason("...password", pass, "my pass");

    expect("extra leading spaces are ignored",
           ap_creds_splitArgs("   Meter hunter2hunter2", ssid, sizeof(ssid),
                              pass, sizeof(pass)), true);
    expectReason("...name still parsed", ssid, "Meter");

    /* A quote inside a quoted field is genuinely ambiguous with no escape
     * syntax, and guessing would store a password nobody typed. Refused. */
    expect("a quote inside a quoted password is refused as ambiguous",
           ap_creds_splitArgs("Meter \"he said \"hi\"\"", ssid, sizeof(ssid),
                              pass, sizeof(pass)), false);

    printf("  -- malformed input must be refused, not guessed at --\n");
    expect("no separator", ap_creds_splitArgs("Meter", ssid, sizeof(ssid),
                                               pass, sizeof(pass)), false);
    expect("name only, no password", ap_creds_splitArgs("Meter ", ssid, sizeof(ssid),
                                                        pass, sizeof(pass)), false);
    expect("unterminated quote in the name",
           ap_creds_splitArgs("\"Meter hunter2hunter2", ssid, sizeof(ssid),
                              pass, sizeof(pass)), false);
    expect("unterminated quote in the password",
           ap_creds_splitArgs("Meter \"hunter2hunter2", ssid, sizeof(ssid),
                              pass, sizeof(pass)), false);
    expect("three fields is a typo, not a password with a space",
           ap_creds_splitArgs("Meter hunter2 oops", ssid, sizeof(ssid),
                              pass, sizeof(pass)), false);
    expect("empty line", ap_creds_splitArgs("", ssid, sizeof(ssid),
                                             pass, sizeof(pass)), false);
    expect("only whitespace", ap_creds_splitArgs("     ", ssid, sizeof(ssid),
                                                 pass, sizeof(pass)), false);
    expect("NULL line", ap_creds_splitArgs(NULL, ssid, sizeof(ssid),
                                           pass, sizeof(pass)), false);
    expectReason("a refused parse leaves the name buffer EMPTY, not garbage",
                 ssid, "");

    /* Overflow. The serial buffer is bounded, so a long paste must not run past
     * the destination - and it must NOT be silently shortened either: truncating
     * a 39-character name to 32 produces a perfectly valid SSID, and the board
     * would broadcast something other than what was typed. The first version of
     * this function truncated, and only this test caught it. */
    {
      char big[AP_MAX_SSID_LEN + 8];
      memset(big, 'N', sizeof(big) - 1);
      big[sizeof(big) - 1] = '\0';
      /* Deliberately generous. A tighter `line` here let snprintf truncate the
       * fixture before the parser saw it, so the "one over the maximum" case
       * silently tested a short password and the whole overflow block proved
       * nothing. */
      char line[256];
      snprintf(line, sizeof(line), "%s hunter2hunter2", big);
      expect("an over-long name is REFUSED, not truncated",
             ap_creds_splitArgs(line, ssid, sizeof(ssid), pass, sizeof(pass)), false);
      expectReason("...and nothing is left behind in the buffers", ssid, "");

      /* The boundary itself still works: exactly 32 octets fits. */
      char exact[AP_MAX_SSID_LEN + 1];
      memset(exact, 'N', AP_MAX_SSID_LEN);
      exact[AP_MAX_SSID_LEN] = '\0';
      snprintf(line, sizeof(line), "%s hunter2hunter2", exact);
      expect("a name of exactly the maximum length is accepted",
             ap_creds_splitArgs(line, ssid, sizeof(ssid), pass, sizeof(pass)), true);
      expectReason("...stored whole", strlen(ssid) == AP_MAX_SSID_LEN ? NULL : "shortened", NULL);

      /* Same for the password: 63 octets fits, 64 does not. */
      char longpass[AP_MAX_PASS_LEN + 2];
      memset(longpass, 'p', AP_MAX_PASS_LEN + 1);
      longpass[AP_MAX_PASS_LEN + 1] = '\0';
      snprintf(line, sizeof(line), "Meter %s", longpass);
      expect("a password one over the maximum is REFUSED",
             ap_creds_splitArgs(line, ssid, sizeof(ssid), pass, sizeof(pass)), false);
      snprintf(line, sizeof(line), "Meter %.*s", AP_MAX_PASS_LEN, longpass);
      expect("a password of exactly the maximum length is accepted",
             ap_creds_splitArgs(line, ssid, sizeof(ssid), pass, sizeof(pass)), true);
      expectReason("...stored whole", strlen(pass) == AP_MAX_PASS_LEN ? NULL : "shortened", NULL);
    }
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
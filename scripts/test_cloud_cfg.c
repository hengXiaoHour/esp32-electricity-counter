/* Host-side unit tests for the cloud (remote-monitoring) config rules.
 *
 * Build + run:
 *   gcc -std=c11 -Wall -Wextra -Isrc/network -x c \
 *       scripts/test_cloud_cfg.c src/network/cloud_cfg.cpp -o /tmp/cloud_cfg_test
 *   /tmp/cloud_cfg_test
 *
 * Exits non-zero if any check failed.
 *
 * The deliberate habits, same as test_ap_creds.c:
 *  1. Boundaries, not vibes: exactly-128 vs 129 char hosts, exactly-256 vs 257
 *     char tokens, and MAC strings that are one char short/long or carry one
 *     bad nibble - because the off-by-one that matters is the one that writes
 *     past a fixed NVS-sized buffer or orphans a database node.
 *  2. A NEGATIVE CONTROL on the harness first: expect("self-check", false,
 *     true) must increment the failure count, or every PASS below is suspect.
 */
#include "cloud_cfg.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void expect(const char *name, bool got, bool want) {
  checks++;
  if (got != want) {
    failures++;
    printf("  FAIL %-42s got %d want %d\n", name, (int)got, (int)want);
  }
}

int main(void) {
  // The harness must be able to fail before any PASS means anything.
  expect("self-check (must fail)", false, true);
  if (failures != 1) {
    printf("  HARNESS BROKEN: negative control did not fail\n");
    return 1;
  }
  failures = 0;
  checks = 0;

  // --- host -----------------------------------------------------------
  expect("typical new-style host", cloud_validateHost("my-proj-default-rtdb.asia-southeast1.firebasedatabase.app"), true);
  expect("typical legacy host", cloud_validateHost("my-proj.firebaseio.com"), true);
  expect("empty host refused", cloud_validateHost(""), false);
  expect("NULL host refused", cloud_validateHost(NULL), false);
  expect("dot-less name refused", cloud_validateHost("localhost"), false);
  expect("URL paste refused (protocol)", cloud_validateHost("https://my-proj.firebaseio.com"), false);
  expect("path paste refused", cloud_validateHost("my-proj.firebaseio.com/devices"), false);
  expect("query paste refused", cloud_validateHost("my-proj.firebaseio.com?auth=x"), false);
  expect("trailing newline refused", cloud_validateHost("my-proj.firebaseio.com\n"), false);
  expect("leading dot refused", cloud_validateHost(".firebaseio.com"), false);
  expect("trailing dot refused", cloud_validateHost("my-proj.firebaseio.com."), false);
  expect("space refused", cloud_validateHost("my proj.firebaseio.com"), false);

  // Boundary: exactly 128 accepted, 129 refused.
  {
    static char h128[CLOUD_MAX_HOST_LEN + 1];
    static char h129[CLOUD_MAX_HOST_LEN + 2];
    memset(h128, 'a', sizeof(h128));
    memset(h129, 'a', sizeof(h129));
    // Keep them dot-valid: overwrite the middle with ".x".
    h128[60] = '.';
    h129[60] = '.';
    h128[CLOUD_MAX_HOST_LEN] = '\0';
    h129[CLOUD_MAX_HOST_LEN + 1] = '\0';
    expect("128-char host accepted", cloud_validateHost(h128), true);
    expect("129-char host refused", cloud_validateHost(h129), false);
  }

  // --- auth token ------------------------------------------------------
  expect("typical token", cloud_validateAuth("AIzaSyD0eX4mpl3T0k3nV4lu3"), true);
  expect("empty token refused", cloud_validateAuth(""), false);
  expect("NULL token refused", cloud_validateAuth(NULL), false);
  expect("trailing newline refused", cloud_validateAuth("token123\n"), false);
  expect("inner space refused", cloud_validateAuth("tok en"), false);
  // Boundary: exactly 256 accepted, 257 refused.
  {
    static char a256[CLOUD_MAX_AUTH_LEN + 1];
    static char a257[CLOUD_MAX_AUTH_LEN + 2];
    memset(a256, 'k', sizeof(a256));
    memset(a257, 'k', sizeof(a257));
    a256[CLOUD_MAX_AUTH_LEN] = '\0';
    a257[CLOUD_MAX_AUTH_LEN + 1] = '\0';
    expect("256-char token accepted", cloud_validateAuth(a256), true);
    expect("257-char token refused", cloud_validateAuth(a257), false);
  }

  // --- herd login: email + password ---------------------------------------
  expect("typical account", cloud_validateEmail("board@esp32.local"), true);
  expect("empty email refused", cloud_validateEmail(""), false);
  expect("NULL email refused", cloud_validateEmail(NULL), false);
  expect("missing at refused", cloud_validateEmail("board.esp32.local"), false);
  expect("double at refused", cloud_validateEmail("a@b@c.local"), false);
  expect("nothing before at refused", cloud_validateEmail("@esp32.local"), false);
  expect("nothing after at refused", cloud_validateEmail("board@"), false);
  expect("no dot after at refused", cloud_validateEmail("board@localhost"), false);
  expect("space refused", cloud_validateEmail("bo ard@esp32.local"), false);
  expect("typical password", cloud_validatePass("correct horse 99!"), true);
  expect("spaces allowed in password", cloud_validatePass("with space"), true);
  expect("empty password refused", cloud_validatePass(""), false);
  expect("NULL password refused", cloud_validatePass(NULL), false);
  expect("newline refused", cloud_validatePass("secret\n"), false);

  // --- three-arg splitter ---------------------------------------------------
  {
    char h[CLOUD_MAX_HOST_LEN + 1], e[CLOUD_MAX_EMAIL_LEN + 1], pw[CLOUD_MAX_PASS_LEN + 1];
    expect("plain three args",
           cloud_splitArgs3("db.firebaseio.com board@esp32.local hunter2", h, sizeof(h), e, sizeof(e), pw, sizeof(pw)), true);
    expect("...host split", strcmp(h, "db.firebaseio.com") == 0, true);
    expect("...email split", strcmp(e, "board@esp32.local") == 0, true);
    expect("...pass split", strcmp(pw, "hunter2") == 0, true);
    expect("password keeps inner spaces",
           cloud_splitArgs3("db.firebaseio.com board@esp32.local correct horse 99!", h, sizeof(h), e, sizeof(e), pw, sizeof(pw))
           && strcmp(pw, "correct horse 99!") == 0, true);
    expect("extra whitespace tolerated",
           cloud_splitArgs3("  db.firebaseio.com   board@esp32.local   hunter2  ", h, sizeof(h), e, sizeof(e), pw, sizeof(pw))
           && strcmp(h, "db.firebaseio.com") == 0 && strcmp(pw, "hunter2") == 0, true);
    expect("missing password refused",
           cloud_splitArgs3("db.firebaseio.com board@esp32.local", h, sizeof(h), e, sizeof(e), pw, sizeof(pw)), false);
    expect("missing email refused",
           cloud_splitArgs3("db.firebaseio.com", h, sizeof(h), e, sizeof(e), pw, sizeof(pw)), false);
    expect("empty refused",
           cloud_splitArgs3("", h, sizeof(h), e, sizeof(e), pw, sizeof(pw)), false);
    expect("over-long host refused, not truncated",
           cloud_splitArgs3("db.firebaseio.com board@esp32.local hunter2", h, 4, e, sizeof(e), pw, sizeof(pw)), false);
  }

  // --- JWT audience readout (cloud diag, never the token) --------------------
  // "eyJhbGciOiJIUzI1NiJ9.eyJhdWQiOiJteS1wcm9qZWN0In0.signature" decodes the
  // middle segment to {"aud":"my-project"}. Built from a real JWT shape.
  {
    // Header {"alg":"HS256"} / payload {"aud":"my-project","x":1} / sig.
    const char *jwt = "eyJhbGciOiJIUzI1NiJ9.eyJhdWQiOiJteS1wcm9qZWN0IiwieCI6MX0.c2ln";
    char aud[64];
    expect("aud extracted", cloud_jwtAud(jwt, aud, sizeof(aud)), true);
    expect("...correct value", strcmp(aud, "my-project") == 0, true);
    expect("two segments refused", cloud_jwtAud("abc.def", aud, sizeof(aud)), false);
    expect("one segment refused", cloud_jwtAud("abc", aud, sizeof(aud)), false);
    expect("bad chars refused", cloud_jwtAud("eyJhbGciOiJIUzI1NiJ9.eyJ!!!QiOiJ4In0.c2ln", aud, sizeof(aud)), false);
    expect("missing aud refused",
           cloud_jwtAud("eyJhbGciOiJIUzI1NiJ9.eyJ4IjoxLCJ6IjoyfQ.c2ln", aud, sizeof(aud)), false);
    expect("NULL jwt refused", cloud_jwtAud(NULL, aud, sizeof(aud)), false);
    expect("tiny buffer refused", cloud_jwtAud(jwt, aud, 4), false);
  }
  {
    char out[16];
    expect("SGVsbG8=less Hello", cloud_b64urlDecode("SGVsbG8", out, sizeof(out)) == 5
           && strcmp(out, "Hello") == 0, true);
    expect("dash/underscore alphabet", cloud_b64urlDecode("Pz4-", out, sizeof(out)) == 3, true);
    expect("empty refused", cloud_b64urlDecode("", out, sizeof(out)) == -1, true);
    expect("len-1-mod-4 refused", cloud_b64urlDecode("abcde", out, sizeof(out)) == -1, true);
    expect("overflow refused", cloud_b64urlDecode("SGVsbG8", out, 4) == -1, true);
  }

  // --- device id from MAC ----------------------------------------------
  {
    char out[CLOUD_DEVICE_ID_LEN + 1];
    memset(out, 0, sizeof(out));
    expect("upper MAC formats", cloud_formatDeviceId("A1:B2:C3:D4:E5:F6", out), true);
    expect("...to 12 upper hex", strcmp(out, "A1B2C3D4E5F6") == 0, true);
    expect("lower MAC upper-cased", cloud_formatDeviceId("a1:b2:c3:d4:e5:f6", out), true);
    expect("...same output", strcmp(out, "A1B2C3D4E5F6") == 0, true);
    expect("short MAC refused", cloud_formatDeviceId("A1:B2:C3:D4:E5", out), false);
    expect("long MAC refused", cloud_formatDeviceId("A1:B2:C3:D4:E5:F6:07", out), false);
    expect("dash separators refused", cloud_formatDeviceId("A1-B2-C3-D4-E5-F6", out), false);
    expect("non-hex nibble refused", cloud_formatDeviceId("A1:B2:C3:D4:E5:FG", out), false);
    expect("NULL mac refused", cloud_formatDeviceId(NULL, out), false);
    expect("NULL out refused", cloud_formatDeviceId("A1:B2:C3:D4:E5:F6", NULL), false);
  }

  printf("  %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

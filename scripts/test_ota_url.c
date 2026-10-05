/* Host-side unit tests for the cloud-OTA download URL rules.
 *
 * Build + run:
 *   gcc -std=c11 -Wall -Wextra -Isrc/network -x c \
 *       scripts/test_ota_url.c src/network/ota_url.cpp -o /tmp/ota_url_test
 *   /tmp/ota_url_test
 *
 * Exits non-zero if any check failed.
 *
 * Habits (same as test_ap_creds.c / test_cloud_cfg.c):
 *  1. Boundaries, not vibes: exactly-220 vs 221 char URLs - because the
 *     off-by-one that matters is the one that writes past the board's
 *     fixed URL buffer.
 *  2. A NEGATIVE CONTROL on the harness first: expect("self-check", false,
 *     true) must increment the failure count, or every PASS below is suspect.
 *  3. Every refusal branch proves its reason is set: a validator that says
 *     "no" with no reason is a dashboard that shrugs at the admin.
 */
#include "ota_url.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void expect(const char *name, bool got, bool want) {
  checks++;
  if (got != want) {
    failures++;
    printf("  FAIL %-46s got %d want %d\n", name, (int)got, (int)want);
  }
}

// Same as expect, but also asserts a refusal carries a reason string.
static void expect_no(const char *name, const char *url) {
  checks++;
  const char *reason = NULL;
  bool got = ota_url_validate(url, &reason);
  if (got || !reason || !*reason) {
    failures++;
    printf("  FAIL %-46s got %d reason %s\n", name, (int)got,
           reason ? reason : "(null)");
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

  const char *good =
      "https://github.com/hengXiaoHour/esp32-electricity-counter"
      "/releases/download/3.2.0/esp32-classic-3.2.0.bin";
  const char *reason = NULL;

  // --- accepts ---------------------------------------------------------
  expect("typical release asset accepted", ota_url_validate(good, &reason), true);
  expect("reason untouched on accept", reason == NULL, true);
  expect("NULL reason arg accepted", ota_url_validate(good, NULL), true);
  expect("tilde in tag accepted",
         ota_url_validate("https://github.com/o/r/releases/download/a~b/f.bin", NULL), true);

  // --- refuses ----------------------------------------------------------
  expect_no("empty refused", "");
  expect_no("NULL refused", NULL);
  expect_no("http downgrade refused",
            "http://github.com/o/r/releases/download/1/f.bin");
  expect_no("tag page refused (HTML, not firmware)",
            "https://github.com/o/r/releases/tag/3.2.0");
  expect_no("repo root refused", "https://github.com/o/r");
  expect_no("lookalike host refused",
            "https://github.com.evil.example/o/r/releases/download/1/f.bin");
  expect_no("gitlab host refused",
            "https://gitlab.com/o/r/releases/download/1/f.bin");
  expect_no("wrong extension refused",
            "https://github.com/o/r/releases/download/1/f.elf");
  expect_no("trailing slash refused (empty file name)",
            "https://github.com/o/r/releases/download/1/.bin/");
  expect_no("space refused",
            "https://github.com/o/r/releases/download/1/my file.bin");
  expect_no("credential paste refused",
            "https://user:pass@github.com/o/r/releases/download/1/f.bin");
  expect_no("query refused",
            "https://github.com/o/r/releases/download/1/f.bin?x=1");
  expect_no("dot-dot refused",
            "https://github.com/o/r/releases/download/../1/f.bin");
  expect_no("percent-encoding refused",
            "https://github.com/o/r/releases/download/1/f%20x.bin");

  // --- boundaries -------------------------------------------------------
  // Exactly OTA_URL_MAX_LEN (220) vs one over. Build by padding the tag.
  char at[OTA_URL_MAX_LEN + 16];
  const char *head = "https://github.com/o/r/releases/download/";
  const char *tail = "/f.bin";
  size_t pad_at = OTA_URL_MAX_LEN - strlen(head) - strlen(tail);
  memset(at, 'a', sizeof(at) - 1);
  at[sizeof(at) - 1] = '\0';
  char ok_url[OTA_URL_MAX_LEN + 16], long_url[OTA_URL_MAX_LEN + 16];
  snprintf(ok_url, sizeof(ok_url), "%s%.*s%s", head, (int)pad_at, at, tail);
  snprintf(long_url, sizeof(long_url), "%s%.*s%s", head, (int)(pad_at + 1), at, tail);
  expect("length accounting sane", strlen(ok_url) == OTA_URL_MAX_LEN, true);
  expect("exactly-220 accepted", ota_url_validate(ok_url, NULL), true);
  expect_no("221 refused", long_url);

  if (failures == 0) {
    printf("  ota_url: all %d checks passed\n", checks);
    return 0;
  }
  printf("  ota_url: %d/%d checks FAILED\n", failures, checks);
  return 1;
}

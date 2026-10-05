// Cloud-OTA download URL rules. Plain C: the host unit test compiles this
// file as C (gcc -x c), so no C++ anywhere in here, only string.h.
#include "ota_url.h"

#include <string.h>

bool ota_url_validate(const char *url, const char **reason) {
  static const char *r_empty = "the URL is empty.";
  static const char *r_long = "the URL is too long (over 220 characters).";
  static const char *r_scheme = "only https:// links are accepted (this one is not).";
  static const char *r_host = "only github.com release links are accepted.";
  static const char *r_shape = "not a release download link: want .../releases/download/<tag>/<file>.bin (a /releases/tag/ page is HTML, not firmware).";
  static const char *r_ext = "the link must end in .bin.";
  static const char *r_chars = "the link has characters a release download URL cannot contain (only letters, digits and / . - _ ~ are allowed).";
  static const char *r_dots = "the link must not contain \"..\" path segments.";
  static const char *r_name = "the file name after the last / is empty.";

#define OTA_FAIL(r) do { if (reason) *reason = (r); return false; } while (0)

  if (!url || !*url) OTA_FAIL(r_empty);

  size_t n = strlen(url);
  if (n > OTA_URL_MAX_LEN) OTA_FAIL(r_long);

  // Reject control bytes, spaces and credential/query pastes anywhere.
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)url[i];
    if (c < 0x21 || c == 0x7f) OTA_FAIL(r_chars);
    if (c == '@' || c == '?' || c == '#') OTA_FAIL(r_chars);
  }

  // Scheme + host, one literal: no http:// downgrade, no lookalike domain
  // (github.com.evil.example slips a mere substring check).
  static const char prefix[] = "https://github.com/";
  if (strncmp(url, prefix, sizeof(prefix) - 1) != 0) {
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) != 0)
      OTA_FAIL(r_scheme);
    OTA_FAIL(r_host);
  }

  const char *path = url + sizeof(prefix) - 1;
  if (strstr(path, "/releases/download/") == NULL) OTA_FAIL(r_shape);

  if (strstr(path, "..") != NULL) OTA_FAIL(r_dots);

  // Extension + non-empty file name. The suffix check runs on the whole
  // URL because '?' and '#' are already refused above, so ".bin" here is
  // really the end of the path, not of a query string.
  if (n < 4 || strcmp(url + n - 4, ".bin") != 0) OTA_FAIL(r_ext);
  const char *slash = strrchr(url, '/');
  if (!slash || !slash[1]) OTA_FAIL(r_name);

  // Path charset after the host: owner/repo/tag/asset names only ever use
  // these. Anything else is a paste error (or an encoding surprise).
  for (const char *p = path; *p; p++) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '/' || c == '.' ||
              c == '-' || c == '_' || c == '~';
    if (!ok) OTA_FAIL(r_chars);
  }

  return true;
}

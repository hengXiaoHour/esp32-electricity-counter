#pragma once
// Cloud-OTA download URL rules, Arduino-free so the host unit test
// scripts/test_ota_url.c can include this directly.
//
// What this is: the board flashes whatever .bin the ADMIN points it at, so
// the URL check is typo-protection, not a security boundary. Trust comes
// from elsewhere: the /cmd downlink only the admin Gmail can write, TLS to
// github.com, and Update.end() validating the image before boot. This module
// just makes "I pasted the wrong link" fail fast with a readable reason
// instead of a 30 s TLS timeout against a nonsense host.
//
// Why so strict (github.com + /releases/download/ + .bin, no query):
// release-asset URLs have exactly that shape, and the one real failure mode
// here is pasting a human page (…/releases/tag/3.2.0, an HTML page that
// Update would happily write to flash and then fail to boot). The strict
// shape refuses the tag page, the repo root, and any http:// downgrade.
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

// Longest release URL this board accepts. A real one is ~90 chars
// (https://github.com/<owner>/<repo>/releases/download/<tag>/<asset>.bin);
// 220 leaves headroom without sizing a stack buffer for a novel.
#define OTA_URL_MAX_LEN 220

// Returns true when `url` is a usable firmware download link. On false,
// *reason (when non-null) points at a static sentence WITHOUT leading
// whitespace. The caller owns the formatting.
bool ota_url_validate(const char *url, const char **reason);

#ifdef __cplusplus
}
#endif

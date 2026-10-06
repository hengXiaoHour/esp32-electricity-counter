#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../config.h"
#include "ota_url.h"

class CloudPush;
class NVSManager;

// Local (LAN) ArduinoOTA plus cloud (GitHub release) updates, one progress
// signal for both: updateLED(), updateEcoMode() and the dashboard's OTA
// banner all read isInProgress()/getProgress(), so whichever transport is
// running shows the same way. Only one transport can run at a time.
//
// Cloud path (serial-first, cloud-ota proven pattern): `update` checks
// version.json and stages the per-chip asset, or `ota <url>` stages a
// direct link; either way the board reboots and the first network tick with
// STA downloads it BEFORE the Firebase sessions exist, against a clean heap.
// 3.2.7 proved a 34 KB largest block cannot finish even an insecure
// handshake at runtime, and freed blocks scatter instead of coalescing - so
// no runtime download is attempted any more. The download uses HTTPUpdate
// over an insecure client (the rig that flashes reliably), with Update.end()
// image validation keeping a bad write from booting. On success the handler
// does NOT reboot itself - it raises cloudRebootDue(), and the sketch routes
// that through ConsoleHandler::requestReboot so the energy counters are
// flushed to NVS first (a reboot that loses a month of counters is not an
// update, it is data loss). On failure the board keeps running with the
// error readable via `ota status` (kept in NVS, so it survives the updater
// reboot too).
class OTAHandler {
public:
  void begin(const char *hostname = "esp32-elec-counter");
  void loop();

  bool isInProgress() const { return inProgress || cloudBusy || cloudActive; }
  uint8_t getProgress() const {
    return (cloudBusy || cloudActive) ? cloudProgress : progress;
  }

  // Validates `url` (github release .bin shape), refuses when the radio has
  // no STA link or another update is running. Returns true when the link
  // was staged: the CALLER persists it to NVS and reboots (see cmdOta).
  // `reply` is one short console line either way (it travels inside the
  // 200-char cloud ack, so it stays terse by construction).
  bool startCloudUpdate(const char *url, bool staUp, String &reply);

  // Serial-first version check (cloud-ota proven pattern): fetches
  // OTA_VERSION_URL (version.json), compares against FIRMWARE_VERSION, and
  // - when doInstall and a newer release exists - validates the per-chip
  // asset URL through the same ota_url_validate gate. Returns true when an
  // image was SELECTED for staging: the CALLER persists stageUrl to NVS
  // and reboots (same path as cmdOta). Small GET only, safe to run inline
  // at verb time even with the SDK sessions up; the firmware download
  // itself still happens in the pre-SDK updater tick. `reply` is one short
  // console line either way.
  bool checkForUpdate(bool doInstall, bool staUp, bool verbose,
                      String &reply, String &stageUrl);

  // Shared version.json fetch behind checkForUpdate and the auto-poll below:
  // true with `latest` + per-chip `binUrl` filled, false on any failure
  // (no STA here - the caller owns that decision). Small GET only.
  bool fetchLatest(String &latest, String &binUrl);

  // Periodic poll, called from loop(): every OTA_CHECK_INTERVAL_MS with STA
  // up it fetches the manifest and - only when a NEWER release appears that
  // this boot has not announced yet - prints one serial line and remembers
  // it for `ota status`. Already-on-latest, bad links and failed fetches
  // stay silent. Never stages, never reboots (notify-only by design).
  void pollTick();

  // True once the poll has seen a newer release (cleared when the manifest
  // no longer names anything newer, e.g. right after flashing it).
  bool updateAvailable() const { return updateAvailable_; }

  // -1/0/1 dotted-decimal compare, cloud-ota compareVersion verbatim
  // ("1.10.0" beats "1.9.9"; missing parts read as 0).
  static int compareVersions(const String &a, const String &b);

  // Store for the NVS staged link + last updater error (wired once at boot).
  // The mutex guards the two commits this makes per attempt (consume +
  // failure record) against sensorTask's energy save on the same handle.
  void setStore(NVSManager *n, SemaphoreHandle_t m) {
    nvs_ = n;
    mutex_ = m;
  }

  // One-line state for `ota status` (serial + both dashboards' consoles).
  // Reads NVS for a staged link / last failure, so it stays truthful across
  // the updater reboot (hence non-const).
  void cloudStatus(String &out);

  // Raised once when a download verified and is ready to boot. The sketch
  // consumes it into a deferred reboot; until then the board keeps running
  // the OLD firmware.
  bool cloudRebootDue() const { return rebootDue; }
  void consumeCloudReboot() { rebootDue = false; }

  // Wired once at boot (.ino): the download entry drops the SDK keep-alive
  // sessions through this before probing github.com (see loopCloud).
  void setCloudPush(CloudPush *c) { cloud_ = c; }

private:
  bool inProgress;
  uint8_t progress;

  // Cloud-download state. cloudBusy also drives isInProgress(), so the LED
  // blinks yellow and eco holds the radio awake for the whole download with
  // no extra wiring.
  bool cloudBusy = false;
  // Stays true for the WHOLE download (cloudBusy is cleared on entry to keep
  // one-attempt semantics). Without this the ota flag lives for one tick and
  // the dashboard banner can never show during a ~60 s fetch.
  bool cloudActive = false;
  bool rebootDue = false;
  uint8_t cloudProgress = 0;
  char cloudUrl[OTA_URL_MAX_LEN + 1] = {0};
  char cloudErr[128] = {0};
  CloudPush *cloud_ = nullptr;
  NVSManager *nvs_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  // RAM copy of a staged link (read once per boot so loop() never polls
  // flash every tick). Set when NVS holds ota_url and cleared on consume.
  bool havePending_ = false;
  char pendUrl_[OTA_URL_MAX_LEN + 1] = {0};
  // One-shot NVS failure record per attempt (no flash churn every tick).
  bool errSaved_ = false;
  // Auto-poll notice (RAM only): the release last announced this boot, the
  // release currently newer than this build, and when the poll last ran.
  // noticedVer_ is the anti-spam latch - one serial line per release.
  bool updateAvailable_ = false;
  char latestVer_[32] = {0};
  char noticedVer_[32] = {0};
  unsigned long lastPollMs_ = 0;

  void loopCloud();
};

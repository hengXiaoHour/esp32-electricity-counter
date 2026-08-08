#include "cloud_ota.h"
#include <Preferences.h>

CloudOTA cloudOta;

// Static trampoline for the RTDB download callback.
static void otaDownloadCallback(RTDB_DownloadStatusInfo info) {
  cloudOta.onStatus(info);
}

CloudOTA::CloudOTA()
  : inProgress(false), progress(0), state("idle") {}

void CloudOTA::begin() {
  Preferences prefs;
  prefs.begin("ota", false);
  appliedMd5 = prefs.getString("appliedMd5", "");
  prefs.end();
  if (appliedMd5.length()) {
    Serial.printf("  CloudOTA: applied md5=%s\n", appliedMd5.c_str());
  }
}

void CloudOTA::loop() {
  // The download runs synchronously in firebaseTask; nothing to do here.
}

void CloudOTA::onStatus(const RTDB_DownloadStatusInfo &info) {
  switch (info.status) {
    case firebase_rtdb_download_status_init:
      state = "starting";
      progress = 0;
      Serial.printf("  [OTA] download init: %s (%d bytes)\n",
                    info.remotePath.c_str(), info.size);
      break;
    case firebase_rtdb_download_status_download:
      progress = (uint8_t)info.progress;
      state = "downloading";
      if ((int)info.progress % 10 == 0) {
        Serial.printf("  [OTA] downloading %u%% (elapsed %d ms)\n",
                      (unsigned)info.progress, info.elapsedTime);
      }
      break;
    case firebase_rtdb_download_status_complete:
      progress = 100;
      state = "done";
      Serial.printf("  [OTA] download complete (%d bytes, %d ms)\n",
                    info.size, info.elapsedTime);
      break;
    case firebase_rtdb_download_status_error:
      state = "failed";
      Serial.printf("  [OTA] download error: %s\n", info.errorMsg.c_str());
      break;
    default:
      break;
  }
}

bool CloudOTA::download(FirebaseData &fb, const String &fwPath, const String &md5) {
  inProgress = true;
  progress = 0;
  state = "starting";
  Serial.printf("\n  [OTA] %s -> RTDB download from %s (free heap %u B)\n",
                FIRMWARE_VERSION, fwPath.c_str(), (unsigned)ESP.getFreeHeap());

  bool ok = Firebase.RTDB.downloadOTA(&fb, fwPath.c_str(), otaDownloadCallback);

  inProgress = false;

  if (!ok) {
    progress = 0;
    state = "failed";
    Serial.printf("  [OTA] downloadOTA failed: code=%d http=%d reason='%s'\n",
                  fb.errorCode(), fb.httpCode(), fb.errorReason().c_str());
    return false;
  }

  if (state != "done") {
    // The completion callback may have reported an error.
    Serial.printf("  [OTA] download ended without completion (state=%s)\n",
                  state.c_str());
    return false;
  }

  Preferences prefs;
  prefs.begin("ota", false);
  prefs.putString("appliedMd5", md5);
  prefs.end();
  appliedMd5 = md5;
  progress = 100;
  state = "done";

  Serial.printf("\n  [OTA] new firmware applied. Rebooting in 2s.\n");
  delay(2000);
  ESP.restart();
  return true;
}
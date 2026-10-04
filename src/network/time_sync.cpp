#include "time_sync.h"
#include "../utils/log_gate.h"

// Defined here so every translation unit shares one clock.

#include <sys/time.h>
#include <time.h>

// Single shared instance. processCommand() is a free function with no state
// of its own, so - exactly like consoleHandler - the clock lives as a global.
TimeSync timeSync;

RTC_DATA_ATTR int64_t TimeSync::rtcLastEpoch = 0;
RTC_DATA_ATTR uint32_t TimeSync::rtcLastMillis = 0;
RTC_DATA_ATTR uint8_t TimeSync::rtcValid = 0;

// Reject a browser clock that is wildly different from what we already believe.
// 180 days of drift is far beyond any real clock error (a phone is within
// seconds) but well under the ~monthly billing cycle, so a bad frame can never
// roll the billing month forwards or backwards by accident.
static constexpr int64_t MAX_JUMP_SECONDS = 15552000LL;  // 180 days

void TimeSync::begin() {
  // RTC memory holds the epoch captured at the last sync plus the millis()
  // reading at that instant. millis() is not preserved across a deep sleep or
  // a power cut, so rtcValid guards against combining a fresh millis() with a
  // stale epoch - that would produce a wildly wrong time.
  if (rtcValid && rtcLastEpoch >= PLAUSIBLE_MIN_EPOCH &&
      rtcLastEpoch <= PLAUSIBLE_MAX_EPOCH) {
    uint32_t elapsedMs = millis() - rtcLastMillis;   // wrap-safe
    struct timeval tv;
    tv.tv_sec = (time_t)(rtcLastEpoch + elapsedMs / 1000);
    tv.tv_usec = (suseconds_t)((elapsedMs % 1000) * 1000);
    settimeofday(&tv, nullptr);
    synced = true;
    lastSyncEpoch = tv.tv_sec;
    lastSyncMillis = millis();
    STATUS_LOG("  [TIME] restored from RTC: epoch %ld (synced %us ago)\n",
               (long)tv.tv_sec, (unsigned)(elapsedMs / 1000));
  } else {
    synced = false;
    STATUS_LOG("  [TIME] no saved clock - waiting for NTP (STA) or a dashboard\n");
  }
}

void TimeSync::beginNTP() {
  if (ntpStarted) return;
  ntpStarted = true;
  // UTC, no DST: the board works in epoch throughout and the browser renders
  // local time, so no timezone is configured here. Three servers so one dead
  // host does not silence the sync.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  STATUS_LOG("  [TIME] NTP started - clock will follow the internet\n");
}

void TimeSync::pollNTP() {
  if (!ntpStarted) return;
  time_t now = time(nullptr);
  if (now < (time_t)PLAUSIBLE_MIN_EPOCH || now > (time_t)PLAUSIBLE_MAX_EPOCH) {
    return;  // SNTP has not written yet (or the link just dropped)
  }
  if (!synced) {
    // First plausible reading: full accept path (jump check is skipped while
    // unsynced, exactly like the first browser frame).
    acceptEpoch((int64_t)now, "NTP");
    return;
  }
  // Already synced: SNTP disciplines the clock in the background, so just
  // re-anchor the age tracker about once a minute. Without this the dashboard
  // would show an ever-growing "set, Ns ago" while the clock is in fact live.
  // No RTC write here - the minute-level freshness is not worth the flash wear
  // on top of what acceptEpoch already persisted.
  int64_t expected = lastSyncEpoch + (int64_t)(millis() - lastSyncMillis) / 1000;
  int64_t drift = (int64_t)now - expected;
  if (drift > 90 || drift < -90) {
    // The live clock disagrees with our track by more than SNTP would ever
    // allow: something else moved it (or it moved). Do NOT silently adopt it -
    // leave the tracker alone so the age indicator goes stale honestly.
    return;
  }
  if ((int64_t)now - lastSyncEpoch >= 60) {
    lastSyncEpoch = (int64_t)now;
    lastSyncMillis = millis();
  }
}

bool TimeSync::acceptEpoch(int64_t epochSeconds, const char *source) {
  if (epochSeconds < PLAUSIBLE_MIN_EPOCH || epochSeconds > PLAUSIBLE_MAX_EPOCH) {
    DEBUG_LOG("  [TIME] rejected implausible %s clock: %lld\n", source,
              (long long)epochSeconds);
    return false;
  }

  // Anti-jitter, shared by both sources: a correct clock is within seconds of
  // ours. A value that disagrees by more than MAX_JUMP_SECONDS is a bug or an
  // attack, and accepting it could move the billing month.
  if (synced) {
    int64_t drift = epochSeconds - (lastSyncEpoch + (millis() - lastSyncMillis) / 1000);
    if (drift > MAX_JUMP_SECONDS || drift < -MAX_JUMP_SECONDS) {
      DEBUG_LOG("  [TIME] rejected %s clock %llds away from ours\n", source,
                (long long)(drift < 0 ? -drift : drift));
      return false;
    }
  }

  lastSyncEpoch = epochSeconds;
  lastSyncMillis = millis();
  synced = true;

  // Persist for the next boot. RTC memory, so this survives ESP.restart().
  rtcLastEpoch = epochSeconds;
  rtcLastMillis = lastSyncMillis;
  rtcValid = 1;

  STATUS_LOG("  [TIME] clock set from %s: epoch %ld\n", source, (long)epochSeconds);
  return true;
}

bool TimeSync::setTimeFromBrowser(int64_t epochSeconds) {
  // The browser path SETS the clock (SNTP is absent in fallback-AP mode), then
  // records through the shared gate. The NTP path never sets - SNTP owns the
  // system time there - it only records via pollNTP().
  if (epochSeconds < PLAUSIBLE_MIN_EPOCH || epochSeconds > PLAUSIBLE_MAX_EPOCH) {
    DEBUG_LOG("  [TIME] rejected implausible browser clock: %lld\n",
              (long long)epochSeconds);
    return false;
  }

  struct timeval tv;
  tv.tv_sec = (time_t)epochSeconds;
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) {
    DEBUG_LOG("  [TIME] settimeofday failed\n");
    return false;
  }

  bool moved = (time(nullptr) != (time_t)epochSeconds);
  bool ok = acceptEpoch(epochSeconds, "dashboard");
  if (ok && !moved) {
    STATUS_LOG("  [TIME] dashboard clock already correct: epoch %ld\n",
               (long)epochSeconds);
  }
  return ok;
}

uint32_t TimeSync::secondsSinceSync() const {
  if (!synced) return 0xFFFFFFFF;  // never synced
  return (millis() - lastSyncMillis) / 1000;
}
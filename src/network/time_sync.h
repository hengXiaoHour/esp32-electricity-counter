#pragma once

#include <Arduino.h>

// Device clock, supplied by whichever browser is connected.
//
// There is no NTP in AP-only mode: configTime() needs a station link, and the
// board has none. That matters far beyond the clock display, because
// LimitManager::rolloverIfNeeded() refuses to act while time(nullptr) <=
// 1600000000. With no clock the monthly billing reset would silently never
// fire, and the event log would carry 1970 timestamps.
//
// The phone does have a correct clock, so the browser lends it: the dashboard
// sends `{"cmd":"set_time","t":<epoch seconds>}` on WebSocket open and
// periodically thereafter.
//
// Two layers of defence, because losing the clock again means losing rollover:
//   1. RTC memory. The last accepted sync is kept in RTC_DATA_ATTR, which
//      survives ESP.restart() (not a power cut). At boot the clock is restored
//      and advanced by however long the board has been up, so rollover keeps
//      working across a restart without a phone attached.
//   2. Plausibility gate. setTimeFromBrowser() rejects absurd values instead
//      of setting the RTC to 1970 or the year 2100.
class TimeSync {
public:
  // Restore the last known time from RTC memory. Call once from setup().
  void begin();

  // Validate and apply a browser-supplied epoch (seconds).
  // Returns true if the clock was moved. Rejects values outside
  // [PLAUSIBLE_MIN, PLAUSIBLE_MAX] so a malformed frame cannot corrupt the
  // RTC, and rejects values wildly far from what we already believe, which
  // would otherwise let a stray frame roll the billing month backwards.
  bool setTimeFromBrowser(int64_t epochSeconds);

  // True once a valid clock has been seen this boot or restored from RTC.
  bool isSynced() const { return synced; }

  // Seconds since the last accepted sync, for the dashboard's staleness hint.
  uint32_t secondsSinceSync() const;

private:
  bool synced = false;
  int64_t lastSyncEpoch = 0;      // epoch at the moment of the last sync
  uint32_t lastSyncMillis = 0;    // millis() at that same moment

  // Kept in RTC memory: survives ESP.restart(), wiped by a power cut.
  // (If a power cut happens, the next browser connection re-syncs anyway.)
  static int64_t rtcLastEpoch;
  static uint32_t rtcLastMillis;
  static uint8_t rtcValid;
};

// Seconds in 1 Jan 2021 .. 1 Jan 2100. Outside this range a "browser clock"
// is a bug or a hostile frame, not a phone.
static constexpr int64_t PLAUSIBLE_MIN_EPOCH = 1609459200LL;   // 2021-01-01
static constexpr int64_t PLAUSIBLE_MAX_EPOCH = 4102444800LL;   // 2100-01-01
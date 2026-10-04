#pragma once

#include <Arduino.h>

// Device clock, supplied by NTP whenever the STA link is up, by whichever
// browser is connected otherwise.
//
// NTP is the primary source: once the home network joins, configTime() points
// SNTP at public servers and the clock disciplines itself in the background -
// no phone needs to be attached for the monthly rollover to fire on time.
// The browser lend (`{"cmd":"set_time","t":<epoch>}` on WebSocket open, then
// periodically) remains as the fallback for the AP-fallback state, which has
// no internet. Whichever speaks first wins; the second must agree with the
// first inside MAX_JUMP_SECONDS, so the two sources can never fight the
// billing month back and forth.
//
// Two more layers of defence, because losing the clock again means losing
// rollover:
//   1. RTC memory. The last accepted sync is kept in RTC_DATA_ATTR, which
//      survives ESP.restart() (not a power cut). At boot the clock is restored
//      and advanced by however long the board has been up, so rollover keeps
//      working across a restart without a phone attached.
//   2. Plausibility gate. Both sources are rejected outside
//      [PLAUSIBLE_MIN, PLAUSIBLE_MAX], and a source that disagrees with the
//      live clock by more than MAX_JUMP_SECONDS is ignored.
class TimeSync {
public:
  // Restore the last known time from RTC memory. Call once from setup().
  void begin();

  // Point SNTP at public servers. Call when the STA link comes up (safe to
  // call repeatedly - only the first call does anything). The actual sync
  // arrives asynchronously; pollNTP() below picks it up.
  void beginNTP();

  // Accept a freshly-arrived NTP time, or re-anchor the age tracker while NTP
  // keeps the clock fresh. Call every networkTask cycle; returns immediately
  // when there is nothing to do. Never moves the clock itself - SNTP owns the
  // system time, this only RECORDS it through the same validation as browser
  // frames, so a rogue NTP reply gets the same rejection as a rogue frame.
  void pollNTP();

  // Validate and apply a browser-supplied epoch (seconds).
  // Returns true if the clock was moved. Rejects values outside
  // [PLAUSIBLE_MIN, PLAUSIBLE_MAX] so a malformed frame cannot corrupt the
  // RTC, and rejects values wildly far from what we already believe, which
  // would otherwise let a stray frame roll the billing month backwards.
  bool setTimeFromBrowser(int64_t epochSeconds);

  // True once a valid clock has been seen this boot or restored from RTC.
  bool isSynced() const { return synced; }

  // True once SNTP has been pointed at servers (STA came up at least once).
  bool ntpActive() const { return ntpStarted; }

  // Seconds since the last accepted sync, for the dashboard's staleness hint.
  uint32_t secondsSinceSync() const;

private:
  // Shared validation + recording for both sources. Returns true if the epoch
  // was accepted (range check, then the jump check against the live clock).
  // Records the sync, persists RTC, and logs - exactly once per accepted value.
  bool acceptEpoch(int64_t epochSeconds, const char *source);

  bool synced = false;
  bool ntpStarted = false;
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

// The one shared clock. processCommand() is a free function, so like
// consoleHandler it is a global rather than an injected dependency.
extern TimeSync timeSync;
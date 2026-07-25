#pragma once

#include "../config.h"
#include "relay_controller.h"

class LimitManager {
public:
  void begin();

  // Called each sensor cycle. Checks all channels against their limits,
  // updates status, trips relays on over-limit, and logs events.
  // powerCalc: provides current/power readings per channel
  void check(const float *currentRMS, const float *activePower,
             const float *powerLimit, const float *currentLimit,
             ChannelData *channels, RelayController &relays,
             Event *events, uint8_t &eventCount);

  // Reset a tripped channel (re-close relay)
  void resetChannel(uint8_t channel, RelayController &relays,
                    ChannelData *channels);

  // Warning threshold percentage (default 90)
  uint8_t warningThreshold;

private:
  bool hasRelay(uint8_t channel) const;
  void logEvent(Event *events, uint8_t &eventCount,
                uint8_t channel, ChannelStatus status,
                float value, const char *msg);
};

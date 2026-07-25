#pragma once

#include "../config.h"

class RelayController {
public:
  void begin();

  // Relay index 0-3 corresponds to channels 1-4
  void set(uint8_t relayIndex, bool on);
  bool getState(uint8_t relayIndex) const;

  // Convenience: trip = off, reset = on
  void trip(uint8_t relayIndex);
  void reset(uint8_t relayIndex);

  // Number of available relays
  uint8_t count() const { return NUM_RELAYS; }

private:
  bool states[NUM_RELAYS];
};

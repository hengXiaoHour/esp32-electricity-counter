#include "relay_controller.h"

void RelayController::begin() {
  for (uint8_t i = 0; i < NUM_RELAYS; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], !RELAY_ACTIVE_STATE);  // Start OFF
    states[i] = false;
  }
}

void RelayController::set(uint8_t relayIndex, bool on) {
  if (relayIndex >= NUM_RELAYS) return;
  digitalWrite(RELAY_PINS[relayIndex], on ? RELAY_ACTIVE_STATE : !RELAY_ACTIVE_STATE);
  states[relayIndex] = on;
}

bool RelayController::getState(uint8_t relayIndex) const {
  if (relayIndex >= NUM_RELAYS) return false;
  return states[relayIndex];
}

void RelayController::trip(uint8_t relayIndex) {
  set(relayIndex, false);
}

void RelayController::reset(uint8_t relayIndex) {
  set(relayIndex, true);
}

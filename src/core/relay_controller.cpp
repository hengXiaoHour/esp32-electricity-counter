#include "relay_controller.h"

void RelayController::begin(const bool *initialStates) {
  for (uint8_t i = 0; i < NUM_RELAYS; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    bool on = initialStates ? initialStates[i] : false;
    digitalWrite(RELAY_PINS[i], on ? RELAY_ACTIVE_STATE : !RELAY_ACTIVE_STATE);
    states[i] = on;
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

bool RelayController::readActualState(uint8_t relayIndex) const {
  if (relayIndex >= NUM_RELAYS) return false;
  return digitalRead(RELAY_PINS[relayIndex]) == RELAY_ACTIVE_STATE;
}

void RelayController::trip(uint8_t relayIndex) {
  set(relayIndex, false);
}

void RelayController::reset(uint8_t relayIndex) {
  set(relayIndex, true);
}

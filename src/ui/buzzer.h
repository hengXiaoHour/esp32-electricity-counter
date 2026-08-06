#pragma once

#include <Arduino.h>

// Non-blocking active-buzzer beep-pattern driver.
//   ring(beeps)  → fast pattern: N beeps (120ms ON / 140ms OFF), then idle.
//   loop()       → millis-based state machine; call every sensorTask cycle (~80ms).
class Buzzer {
public:
  void begin(uint8_t pin);

  // Ring N beeps. If called while busy, the pattern restarts from the first beep.
  void ring(uint8_t beeps);

  // Immediately silence the buzzer and cancel any running pattern.
  void stop();

  void loop();

  bool isBusy() const { return state != IDLE; }

private:
  static const uint16_t BEEP_MS = 120;
  static const uint16_t GAP_MS = 140;

  enum BeepState : uint8_t {
    IDLE = 0,
    BEEP_ON = 1,
    BEEP_GAP = 2
  };

  uint8_t pin;
  BeepState state;
  uint8_t beepsRemaining;
  uint32_t lastChange;
};

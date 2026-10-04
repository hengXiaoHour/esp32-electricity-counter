#pragma once

#include <Arduino.h>

// Non-blocking active-buzzer beep-pattern driver.
//   ring(beeps)  → fast pattern: N beeps (80ms ON / 80ms OFF), then idle.
//   loop()       → millis-based state machine; call every sensorTask cycle (~80ms).
//
// Kept SHORT on purpose: the trip pattern repeats the channel number in beeps,
// so channel 5 means 5 beeps. At the old 120/140ms a round took well over a
// second and channels 2-5 all blurred together; at 80/80ms a full round is
// 0.8s and the count is easy to follow. Multiples of the 80ms sensor cycle,
// so the timings land exactly instead of rounding up to the next tick.
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
  static const uint16_t BEEP_MS = 80;
  static const uint16_t GAP_MS = 80;

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

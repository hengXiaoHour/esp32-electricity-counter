#pragma once

#include <Arduino.h>

// Non-blocking active-buzzer beep-pattern driver.
//   ring(beeps)  → fast pattern: N beeps (40ms ON / 40ms OFF), then a 1s
//                  end-pause, then idle.
//   loop()       → millis-based state machine. Called from BOTH tasks (sensor
//                  80ms + network 20ms), so the effective tick is 20ms and the
//                  40ms timings land exactly. Double-calling is safe: the
//                  second call in the same tick sees the already-advanced state
//                  and does nothing (worst case across cores is one skipped beep
//                  in a pattern that replays forever while tripped, inaudible).
//
// The 1s end-pause is what keeps multi-channel trips countable: without it the
// next pattern would start ~80ms after the last beep and ch2+ch3 back-to-back
// would sound like one 5-beep pattern. With it every round is
// pattern → 1s silence → pattern, per channel in turn.
//
// Kept SHORT on purpose: the trip pattern repeats the channel number in beeps,
// so channel 5 means 5 beeps. 40/40ms puts the beeps at 0.4s for a full round,
// and the count stays easy to follow. Anything much shorter blurs the gaps
// between beeps and counting gets harder, not easier.
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
  static const uint16_t BEEP_MS = 40;
  static const uint16_t GAP_MS = 40;
  // Silence after the last beep before the driver reports idle. updateBuzzer()
  // waits on isBusy(), so this pause separates every pattern round - and every
  // channel's turn - by a full second of silence.
  static const uint16_t END_PAUSE_MS = 1000;

  enum BeepState : uint8_t {
    IDLE = 0,
    BEEP_ON = 1,
    BEEP_GAP = 2,
    END_PAUSE = 3
  };

  uint8_t pin;
  BeepState state;
  uint8_t beepsRemaining;
  uint32_t lastChange;
};

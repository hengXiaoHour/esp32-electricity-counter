#include "buzzer.h"

void Buzzer::begin(uint8_t pinArg) {
  pin = pinArg;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  state = IDLE;
  beepsRemaining = 0;
  lastChange = 0;
}

void Buzzer::ring(uint8_t beeps) {
  if (beeps == 0) return;
  // Restart-from-first-beep, even mid-pause: a new trip interrupts the silence
  // rather than queueing behind it.
  state = BEEP_ON;
  beepsRemaining = beeps;
  lastChange = millis();
  digitalWrite(pin, HIGH);
}

void Buzzer::stop() {
  state = IDLE;
  beepsRemaining = 0;
  digitalWrite(pin, LOW);
}

void Buzzer::loop() {
  uint32_t now = millis();
  switch (state) {
    case BEEP_ON:
      if (now - lastChange >= BEEP_MS) {
        lastChange = now;
        digitalWrite(pin, LOW);
        state = BEEP_GAP;
      }
      break;

    case BEEP_GAP:
      if (now - lastChange >= GAP_MS) {
        lastChange = now;
        if (beepsRemaining > 0) {
          beepsRemaining--;
          if (beepsRemaining > 0) {
            digitalWrite(pin, HIGH);
            state = BEEP_ON;
          } else {
            // Last beep done: hold a full second of silence before reporting
            // idle, so the next pattern (same channel repeating, or the next
            // tripped channel) starts cleanly instead of blurring into this one.
            digitalWrite(pin, LOW);
            state = END_PAUSE;
          }
        } else {
          digitalWrite(pin, LOW);
          state = END_PAUSE;
        }
      }
      break;

    case END_PAUSE:
      if (now - lastChange >= END_PAUSE_MS) {
        state = IDLE;
      }
      break;

    case IDLE:
      break;
  }
}

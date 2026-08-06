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
            digitalWrite(pin, LOW);
            state = IDLE;
          }
        } else {
          digitalWrite(pin, LOW);
          state = IDLE;
        }
      }
      break;

    case IDLE:
      break;
  }
}

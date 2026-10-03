#include "status_led.h"

StatusLED::StatusLED()
  : strip(1, PIN_RGB_LED, NEO_GRB + NEO_KHZ800),
    currentMode(LED_OFF),
    blinkState(false),
    lastToggle(0),
    rgbMode(false) {}  // factory default: plain, non-RGB LED

void StatusLED::setType(bool rgb) {
  rgbMode = rgb;
  // Either way the pin starts dark; begin()/loop() take it from there.
  if (rgb) {
    strip.begin();
    strip.setBrightness(50);
    strip.show();
  } else {
    pinMode(PIN_RGB_LED, OUTPUT);
    digitalWrite(PIN_RGB_LED, LOW);
  }
}

void StatusLED::begin() {
  setType(rgbMode);
}

void StatusLED::setMode(LedMode mode) {
  if (currentMode != mode) {
    currentMode = mode;
    blinkState = false;
    lastToggle = 0;
  }
}

void StatusLED::loop() {
  if (!rgbMode) {
    // Plain LED has exactly two states; every solid maps to on, every blink
    // toggles on its own interval.
    switch (currentMode) {
      case LED_OFF:
        digitalWrite(PIN_RGB_LED, LOW);
        break;
      case LED_SOLID_GREEN:
      case LED_SOLID_RED:
      case LED_SOLID_BLUE:
        digitalWrite(PIN_RGB_LED, HIGH);
        break;
      case LED_BLINK_YELLOW:
        if (millisSinceLastToggle() >= 500) {
          blinkState = !blinkState;
          lastToggle = millis();
        }
        digitalWrite(PIN_RGB_LED, blinkState ? HIGH : LOW);
        break;
      case LED_BLINK_RED:
        if (millisSinceLastToggle() >= 300) {
          blinkState = !blinkState;
          lastToggle = millis();
        }
        digitalWrite(PIN_RGB_LED, blinkState ? HIGH : LOW);
        break;
    }
    return;
  }

  switch (currentMode) {
    case LED_OFF:
      setPixelColor(0, 0, 0);
      break;

    case LED_SOLID_GREEN:
      setPixelColor(0, 10, 0);
      break;

    case LED_SOLID_RED:
      setPixelColor(25, 0, 0);
      break;

    case LED_SOLID_BLUE:
      setPixelColor(0, 0, 25);
      break;

    case LED_BLINK_YELLOW:
      if (millisSinceLastToggle() >= 500) {
        blinkState = !blinkState;
        lastToggle = millis();
      }
      if (blinkState) {
        setPixelColor(25, 25, 0);
      } else {
        setPixelColor(0, 0, 0);
      }
      break;

    case LED_BLINK_RED:
      if (millisSinceLastToggle() >= 300) {
        blinkState = !blinkState;
        lastToggle = millis();
      }
      if (blinkState) {
        setPixelColor(255, 0, 0);
      } else {
        setPixelColor(0, 0, 0);
      }
      break;
  }
  strip.show();
}

// R and G are physically swapped on this board's WS2812.
// We swap them so caller-visible API is normal RGB.
void StatusLED::setPixelColor(uint8_t r, uint8_t g, uint8_t b) {
  // NOTE: R/G swap – do not "fix" this, it's intentional for this PCB layout.
  strip.setPixelColor(0, strip.Color(g, r, b));
}

uint32_t StatusLED::millisSinceLastToggle() {
  return millis() - lastToggle;
}

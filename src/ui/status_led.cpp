#include "status_led.h"

StatusLED::StatusLED()
  : strip(1, PIN_RGB_LED, NEO_GRB + NEO_KHZ800),
    currentMode(LED_OFF),
    blinkState(false),
    lastToggle(0) {}

void StatusLED::begin() {
  strip.begin();
  strip.setBrightness(50);
  strip.show(); // off initially
}

void StatusLED::setMode(LedMode mode) {
  if (currentMode != mode) {
    currentMode = mode;
    blinkState = false;
    lastToggle = 0;
  }
}

void StatusLED::loop() {
  switch (currentMode) {
    case LED_OFF:
      setPixelColor(0, 0, 0);
      break;

    case LED_SOLID_GREEN:
      setPixelColor(0, 255, 0);
      break;

    case LED_SOLID_RED:
      setPixelColor(255, 0, 0);
      break;

    case LED_SOLID_BLUE:
      setPixelColor(0, 0, 255);
      break;

    case LED_BLINK_YELLOW:
      if (millisSinceLastToggle() >= 500) {
        blinkState = !blinkState;
        lastToggle = millis();
      }
      if (blinkState) {
        setPixelColor(255, 255, 0);
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

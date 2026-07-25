#pragma once

#include <Adafruit_NeoPixel.h>
#include "../config.h"

// LED State Machine
//   Solid GREEN  – WiFi connected, all channels OK
//   Blink YELLOW – Any channel ≥ WARNING_THRESHOLD_PCT of limit
//   Solid RED    – WiFi disconnected / AP fallback mode
//   Blink RED    – A channel tripped (relay cut)
//   Solid BLUE   – OTA update in progress / booting
//   OFF          – System off / deep sleep

enum LedMode : uint8_t {
  LED_OFF = 0,
  LED_SOLID_GREEN,
  LED_BLINK_YELLOW,
  LED_SOLID_RED,
  LED_BLINK_RED,
  LED_SOLID_BLUE
};

class StatusLED {
public:
  StatusLED();
  void begin();
  void setMode(LedMode mode);
  LedMode getMode() const { return currentMode; }
  void loop();

private:
  Adafruit_NeoPixel strip;
  LedMode currentMode;
  bool blinkState;
  uint32_t lastToggle;

  // R/G channel swap wrapper:
  // Hardware has physical R and G swapped.
  // Caller uses normal (r,g,b) – green(0,255,0) → sent as (255,0,0) to hardware.
  void setPixelColor(uint8_t r, uint8_t g, uint8_t b);
  uint32_t millisSinceLastToggle();
};

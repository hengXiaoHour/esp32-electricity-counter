#pragma once

#include <Adafruit_NeoPixel.h>
#include "../config.h"

// LED State Machine (firmware logic uses only OFF / SOLID_GREEN / BLINK_YELLOW)
//   OFF          – idle: AP up, no client
//   SOLID_GREEN  – an AP client is connected
//   BLINK_YELLOW – OTA in progress (toggles every 500 ms)
//
// Two physical driver types, same GPIO48:
//   normal – plain on/off LED on GPIO48 (factory default)
//   rgb    – WS2812 on GPIO48 (R/G swapped on this PCB, compensated below)

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

  // Select the physical LED type. Default (constructor + fresh NVS) is the
  // plain, non-RGB LED. Takes effect immediately; call before begin() at boot,
  // or any time to switch live.
  void setType(bool rgb);
  bool isRgb() const { return rgbMode; }

private:
  Adafruit_NeoPixel strip;
  LedMode currentMode;
  bool blinkState;
  uint32_t lastToggle;
  bool rgbMode;

  // R/G channel swap wrapper:
  // Hardware has physical R and G swapped.
  // Caller uses normal (r,g,b) – green(0,255,0) → sent as (255,0,0) to hardware.
  void setPixelColor(uint8_t r, uint8_t g, uint8_t b);
  uint32_t millisSinceLastToggle();
};

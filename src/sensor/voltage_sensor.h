#pragma once

#include "../config.h"

class VoltageSensor {
public:
  void begin();

  // Collects RMS_SAMPLES from the voltage pin and returns RMS voltage
  float readRMS();

  // Calibration multiplier (display volts per volt measured)
  float calibration;

private:
  void sampleVoltage(float *samples);
  float computeRMS(const float *samples, int count);
};

#pragma once

#include "../config.h"

class CurrentSensor {
public:
  void begin();

  // Interleaved sampling: collects MAX_RMS_SAMPLES from each channel
  // Results written to outRMS array (length numChannels, must be NUM_CHANNELS)
  void readAll(float *outRMS, uint8_t numChannels = NUM_CHANNELS);

  // Calibration multiplier (amps per volt measured)
  float calibration;

private:
  void sampleAllChannels(float samples[NUM_CHANNELS][MAX_RMS_SAMPLES]);
  float computeRMS(const float *samples, int count);
};

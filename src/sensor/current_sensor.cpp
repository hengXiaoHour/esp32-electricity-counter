#include "current_sensor.h"

void CurrentSensor::begin() {
  calibration = DEFAULT_CURRENT_CALIBRATION;
  analogReadResolution(ADC_RESOLUTION);
}

void CurrentSensor::readAll(float *outRMS, uint8_t numChannels) {
  // Buffer: transposed so samples[ch][i] for all ch first, then next i
  float samples[NUM_CHANNELS][RMS_SAMPLES];

  sampleAllChannels(samples);

  for (uint8_t ch = 0; ch < numChannels; ch++) {
    outRMS[ch] = computeRMS(samples[ch], RMS_SAMPLES);
  }
}

void CurrentSensor::sampleAllChannels(float samples[NUM_CHANNELS][RMS_SAMPLES]) {
  // Interleaved sampling: one ADC read per channel per cycle
  // This keeps samples evenly spread across the AC waveform
  for (int i = 0; i < RMS_SAMPLES; i++) {
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
      samples[ch][i] = (float)analogRead(CURRENT_PINS[ch]);
      delayMicroseconds(ADC_READ_INTERVAL_US);
    }
  }
}

float CurrentSensor::computeRMS(const float *samples, int count) {
  float sum = 0.0f;
  float sumSq = 0.0f;

  for (int i = 0; i < count; i++) {
    sum += samples[i];
    sumSq += samples[i] * samples[i];
  }

  float mean = sum / count;
  float meanSq = sumSq / count;
  float variance = meanSq - mean * mean;

  if (variance < 0.0f) variance = 0.0f;
  float adcRMS = sqrtf(variance);

  // Convert ADC units to voltage at the pin
  float voltageRMS = (adcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V;

  // Convert voltage to current using calibration factor
  // SCT-013-100: 1V RMS output = 100A RMS → calibration = 100.0
  float currentRMS = voltageRMS * calibration;

  return currentRMS;
}

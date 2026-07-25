#include "voltage_sensor.h"

void VoltageSensor::begin() {
  calibration = DEFAULT_VOLTAGE_CALIBRATION;
  analogReadResolution(ADC_RESOLUTION);
}

float VoltageSensor::readRMS() {
  float samples[RMS_SAMPLES];
  sampleVoltage(samples);
  float adcRMS = computeRMS(samples, RMS_SAMPLES);

  // Convert ADC units to voltage at the pin
  float voltageRMS = (adcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V;

  // Apply calibration to get actual mains voltage
  return voltageRMS * calibration;
}

void VoltageSensor::sampleVoltage(float *samples) {
  for (int i = 0; i < RMS_SAMPLES; i++) {
    samples[i] = (float)analogRead(PIN_VOLTAGE);
    delayMicroseconds(ADC_READ_INTERVAL_US);
  }
}

float VoltageSensor::computeRMS(const float *samples, int count) {
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
  return sqrtf(variance);
}

#include "power_calculator.h"
#include <math.h>

void PowerCalculator::begin() {
  voltageCal = DEFAULT_VOLTAGE_CALIBRATION;
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    currentCal[ch] = DEFAULT_CURRENT_CALIBRATION;
    energyKWh[ch] = 0.0f;
  }
  analogReadResolution(ADC_RESOLUTION);
}

void PowerCalculator::update(float deltaSeconds) {
  collectSamples();
  computeAll();

  // Accumulate energy (kWh)
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    // activePower is in Watts, deltaSeconds is in seconds
    // Energy (kWh) = Power (W) × Time (h) / 1000
    float deltaHours = deltaSeconds / 3600.0f;
    energyKWh[ch] += activePower[ch] * deltaHours / 1000.0f;
  }
}

void PowerCalculator::collectSamples() {
  // Interleaved sampling: voltage + all 6 current channels per index
  // This keeps V-I samples closely paired for real power calculation
  for (int i = 0; i < RMS_SAMPLES; i++) {
    voltageSamples[i] = (float)analogRead(PIN_VOLTAGE);
    delayMicroseconds(ADC_READ_INTERVAL_US);

    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
      currentSamples[ch][i] = (float)analogRead(CURRENT_PINS[ch]);
      delayMicroseconds(ADC_READ_INTERVAL_US);
    }
  }
}

void PowerCalculator::computeAll() {
  // 1. Remove DC offset from voltage
  float vSum = 0.0f;
  for (int i = 0; i < RMS_SAMPLES; i++) {
    vSum += voltageSamples[i];
  }
  float vMean = vSum / RMS_SAMPLES;

  // Compute voltage RMS
  float vSumSq = 0.0f;
  for (int i = 0; i < RMS_SAMPLES; i++) {
    float centered = voltageSamples[i] - vMean;
    vSumSq += centered * centered;
  }
  float vVariance = vSumSq / RMS_SAMPLES;
  float vAdcRMS = sqrtf(vVariance > 0.0f ? vVariance : 0.0f);
  float vPinVoltage = (vAdcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V;
  voltageRMS = vPinVoltage * voltageCal;

  // 2. Per-channel: current RMS, real power, apparent power, PF
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    float iSum = 0.0f;
    float iSumSq = 0.0f;
    float pSum = 0.0f;  // Instantaneous power = v_centered × i_centered

    for (int i = 0; i < RMS_SAMPLES; i++) {
      float vCentered = voltageSamples[i] - vMean;
      float iCentered = currentSamples[ch][i];

      iSum += currentSamples[ch][i];
      iSumSq += currentSamples[ch][i] * currentSamples[ch][i];
      pSum += vCentered * iCentered;
    }

    // Current RMS (remove DC offset)
    float iMean = iSum / RMS_SAMPLES;
    float iMeanSq = iSumSq / RMS_SAMPLES;
    float iVariance = iMeanSq - iMean * iMean;
    float iAdcRMS = sqrtf(iVariance > 0.0f ? iVariance : 0.0f);
    float iPinVoltage = (iAdcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V;
    currentRMS[ch] = iPinVoltage * currentCal[ch];

    // Real power: average of instantaneous V×I (with DC offsets removed)
    float pMean = pSum / RMS_SAMPLES;
    // Convert from ADC units to real Watts:
    // (adc_v - vMean) represents voltage swing at pin (0-3.3V → 0-ADC_MAX_VALUE)
    // (adc_i - iMean) represents current sensor output voltage
    // Real power in watts requires the product of actual voltage × actual current
    float pinVoltageScale = ADC_REFERENCE_V / ADC_MAX_VALUE;
    float pWatts = pMean * pinVoltageScale * voltageCal * currentCal[ch];
    activePower[ch] = fabsf(pWatts);

    // Apparent power
    float vActual = (vAdcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V * voltageCal;
    float iActual = currentRMS[ch];
    apparentPower[ch] = vActual * iActual;

    // Power factor
    if (apparentPower[ch] > 0.001f) {
      powerFactor[ch] = activePower[ch] / apparentPower[ch];
      if (powerFactor[ch] > 1.0f) powerFactor[ch] = 1.0f;
    } else {
      powerFactor[ch] = 1.0f;
    }
  }
}

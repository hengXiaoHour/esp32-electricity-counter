#include "power_calculator.h"
#include <math.h>

void PowerCalculator::begin() {
  voltageCal = DEFAULT_VOLTAGE_CALIBRATION;
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    currentCal[ch] = DEFAULT_CURRENT_CALIBRATION;
    energyKWh[ch] = 0.0f;
    noiseFloor[ch] = 0.0f;
    lpfAlpha[ch] = 1.0f;
    filteredCurrentRMS[ch] = 0.0f;
  }
  autoZeroPending = false;
  analogReadResolution(ADC_RESOLUTION);
}

void PowerCalculator::update(float deltaSeconds) {
  collectSamples();
  computeAll();

  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    float deltaHours = deltaSeconds / 3600.0f;
    energyKWh[ch] += activePower[ch] * deltaHours / 1000.0f;
  }
}

void PowerCalculator::collectSamples() {
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
  float vSum = 0.0f;
  for (int i = 0; i < RMS_SAMPLES; i++) {
    vSum += voltageSamples[i];
  }
  float vMean = vSum / RMS_SAMPLES;

  float vSumSq = 0.0f;
  for (int i = 0; i < RMS_SAMPLES; i++) {
    float centered = voltageSamples[i] - vMean;
    vSumSq += centered * centered;
  }
  float vVariance = vSumSq / RMS_SAMPLES;
  float vAdcRMS = sqrtf(vVariance > 0.0f ? vVariance : 0.0f);
  float vPinVoltage = (vAdcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V;
  voltageRMS = vPinVoltage * voltageCal;

  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    float iSum = 0.0f;
    float iSumSq = 0.0f;
    float pSum = 0.0f;

    for (int i = 0; i < RMS_SAMPLES; i++) {
      float vCentered = voltageSamples[i] - vMean;
      float iCentered = currentSamples[ch][i];

      iSum += currentSamples[ch][i];
      iSumSq += currentSamples[ch][i] * currentSamples[ch][i];
      pSum += vCentered * iCentered;
    }

    float iMean = iSum / RMS_SAMPLES;
    float iMeanSq = iSumSq / RMS_SAMPLES;
    float iVariance = iMeanSq - iMean * iMean;
    float iAdcRMS = sqrtf(iVariance > 0.0f ? iVariance : 0.0f);
    float iPinVoltage = (iAdcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V;
    float rawRMS = iPinVoltage * currentCal[ch];

    float adjustedRMS = rawRMS - noiseFloor[ch];
    if (adjustedRMS < 0.0f) adjustedRMS = 0.0f;

    currentRMS[ch] = adjustedRMS;

    filteredCurrentRMS[ch] = lpfAlpha[ch] * adjustedRMS + (1.0f - lpfAlpha[ch]) * filteredCurrentRMS[ch];

    float pMean = pSum / RMS_SAMPLES;
    float pinVoltageScale = ADC_REFERENCE_V / ADC_MAX_VALUE;
    float pWatts = pMean * pinVoltageScale * voltageCal * currentCal[ch];
    activePower[ch] = fabsf(pWatts);

    float vActual = (vAdcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V * voltageCal;
    float iActual = filteredCurrentRMS[ch];
    apparentPower[ch] = vActual * iActual;

    if (apparentPower[ch] > 0.001f) {
      powerFactor[ch] = activePower[ch] / apparentPower[ch];
      if (powerFactor[ch] > 1.0f) powerFactor[ch] = 1.0f;
    } else {
      powerFactor[ch] = 1.0f;
    }
  }
}

void PowerCalculator::setNoiseFloor(int ch, float val) {
  if (ch < 0 || ch >= NUM_CHANNELS) return;
  noiseFloor[ch] = val;
  filteredCurrentRMS[ch] = 0.0f;
}

void PowerCalculator::setLpfAlpha(int ch, float val) {
  if (ch < 0 || ch >= NUM_CHANNELS) return;
  if (val < 0.01f) val = 0.01f;
  if (val > 1.0f) val = 1.0f;
  lpfAlpha[ch] = val;
}

void PowerCalculator::requestAutoZero(int ch) {
  if (ch < 0 || ch >= NUM_CHANNELS) return;
  autoZeroChannel = ch;
  autoZeroPending = true;
}

float PowerCalculator::runAutoZeroSingle(int ch) {
  collectSamples();
  computeAll();
  return currentRMS[ch];
}

#include "power_calculator.h"
#include <math.h>

void PowerCalculator::begin() {
  rmsSamples = MAX_RMS_SAMPLES / 2;
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
  float vFiltered = 0.0f;
  float cFiltered[NUM_CHANNELS];
  bool vInit = false;
  bool cInit[NUM_CHANNELS] = {false};
  float vAlpha = lpfAlpha[0];

  for (int i = 0; i < rmsSamples; i++) {
    float vRaw = (float)analogRead(PIN_VOLTAGE);
    if (vAlpha < 1.0f) {
      if (!vInit) { vFiltered = vRaw; vInit = true; }
      vFiltered += vAlpha * (vRaw - vFiltered);
      voltageSamples[i] = vFiltered;
    } else {
      voltageSamples[i] = vRaw;
    }

    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
      float raw = (float)analogRead(CURRENT_PINS[ch]);
      float alpha = lpfAlpha[ch];
      if (alpha < 1.0f) {
        if (!cInit[ch]) { cFiltered[ch] = raw; cInit[ch] = true; }
        cFiltered[ch] += alpha * (raw - cFiltered[ch]);
        currentSamples[ch][i] = cFiltered[ch];
      } else {
        currentSamples[ch][i] = raw;
      }
    }

    delayMicroseconds(ADC_READ_INTERVAL_US);
  }
}

void PowerCalculator::computeAll() {
  float vSum = 0.0f;
  for (int i = 0; i < rmsSamples; i++) {
    vSum += voltageSamples[i];
  }
  float vMean = vSum / rmsSamples;

  float vSumSq = 0.0f;
  for (int i = 0; i < rmsSamples; i++) {
    float centered = voltageSamples[i] - vMean;
    vSumSq += centered * centered;
  }
  float vVariance = vSumSq / rmsSamples;
  float vAdcRMS = sqrtf(vVariance > 0.0f ? vVariance : 0.0f);
  float vPinVoltage = (vAdcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V;
  voltageRMS = vPinVoltage * voltageCal;

  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    float iSum = 0.0f;
    float iSumSq = 0.0f;
    float pSum = 0.0f;

    for (int i = 0; i < rmsSamples; i++) {
      float vCentered = voltageSamples[i] - vMean;
      float iCentered = currentSamples[ch][i];

      iSum += currentSamples[ch][i];
      iSumSq += currentSamples[ch][i] * currentSamples[ch][i];
      pSum += vCentered * iCentered;
    }

    float iMean = iSum / rmsSamples;
    float iMeanSq = iSumSq / rmsSamples;
    float iVariance = iMeanSq - iMean * iMean;
    float iAdcRMS = sqrtf(iVariance > 0.0f ? iVariance : 0.0f);
    float iPinVoltage = (iAdcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V;
    float rawRMS = iPinVoltage * currentCal[ch];

    float rawSq = rawRMS * rawRMS;
    float floorSq = noiseFloor[ch] * noiseFloor[ch];
    float signalRMS = (rawSq > floorSq) ? sqrtf(rawSq - floorSq) : 0.0f;

    currentRMS[ch] = signalRMS;
    filteredCurrentRMS[ch] = signalRMS;

    float pMean = pSum / rmsSamples;
    float adcToVolt = ADC_REFERENCE_V / ADC_MAX_VALUE;
    float pWatts = pMean * adcToVolt * adcToVolt * voltageCal * currentCal[ch];
    float noisePower = voltageRMS * noiseFloor[ch];
    activePower[ch] = fabsf(pWatts) - noisePower;
    if (activePower[ch] < 0.0f) activePower[ch] = 0.0f;

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
  float savedNF = noiseFloor[ch];
  noiseFloor[ch] = 0.0f;
  collectSamples();
  computeAll();
  float rawRMS = currentRMS[ch];
  noiseFloor[ch] = savedNF;
  return rawRMS;
}

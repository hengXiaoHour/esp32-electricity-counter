#include "power_calculator.h"
#include <math.h>

void PowerCalculator::begin() {
  rmsSamples = MAX_RMS_SAMPLES / 2;
  voltageCal = DEFAULT_VOLTAGE_CALIBRATION;
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    currentCal[ch] = DEFAULT_CURRENT_CALIBRATION;
    energyKWh[ch] = 0.0f;
    noiseFloor[ch] = 0.0f;
    lpfAlpha[ch] = DEFAULT_LPF_ALPHA;
    filteredCurrentRMS[ch] = 0.0f;
    rmsInit[ch] = false;
  }
  azQueueLen = 0;
  azActive = false;
  azLpfForced = false;
  azChannel = -1;
  azBatchCount = 0;
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
  for (int i = 0; i < rmsSamples; i++) {
    voltageSamples[i] = (float)analogRead(PIN_VOLTAGE);

    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
      currentSamples[ch][i] = (float)analogRead(CURRENT_PINS[ch]);
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

  // The current channels need their means BEFORE the variance can be summed,
  // so this is a separate first pass. Splitting the loop is what removes the
  // float32 cancellation of E[X^2] - E[X]^2: at the ~2048-count mid-supply
  // bias, E[X^2] is ~4.19e6 and one float32 ULP there is 0.5 counts^2, while
  // the variance of a 0.01 A load is ~0.01 counts^2. The one-pass form returned
  // rounding noise instead of the signal; see doc/design.md for the measured
  // before/after. (The voltage path above was already two-pass.)
  float iMean[NUM_CHANNELS];
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    float iSum = 0.0f;
    for (int i = 0; i < rmsSamples; i++) iSum += currentSamples[ch][i];
    iMean[ch] = iSum / rmsSamples;
  }

  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    float iSumSq = 0.0f;
    float pSum = 0.0f;

    for (int i = 0; i < rmsSamples; i++) {
      float vCentered = voltageSamples[i] - vMean;
      // Genuinely centered now. It was named iCentered while still holding the
      // raw sample; the covariance was still correct because vCentered sums to
      // zero, but the name invited the reader to assume a trim that was absent.
      float iCentered = currentSamples[ch][i] - iMean[ch];

      iSumSq += iCentered * iCentered;
      pSum += vCentered * iCentered;
    }

    float iVariance = iSumSq / rmsSamples;
    float iAdcRMS = sqrtf(iVariance > 0.0f ? iVariance : 0.0f);
    float iPinVoltage = (iAdcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V;
    float rawRMS = iPinVoltage * currentCal[ch];

    float rawSq = rawRMS * rawRMS;
    float floor = (azActive && ch == azChannel) ? 0.0f : noiseFloor[ch];
    float floorSq = floor * floor;
    float signalRMS = (rawSq > floorSq) ? sqrtf(rawSq - floorSq) : 0.0f;

    currentRMS[ch] = signalRMS;

    float alpha = azLpfForced ? 1.0f : lpfAlpha[ch];
    if (alpha < 1.0f) {
      if (!rmsInit[ch]) { filteredCurrentRMS[ch] = signalRMS; rmsInit[ch] = true; }
      filteredCurrentRMS[ch] += alpha * (signalRMS - filteredCurrentRMS[ch]);
    } else {
      filteredCurrentRMS[ch] = signalRMS;
    }

    float pMean = pSum / rmsSamples;
    float adcToVolt = ADC_REFERENCE_V / ADC_MAX_VALUE;
    float pWatts = pMean * adcToVolt * adcToVolt * voltageCal * currentCal[ch];
    // NO noise-power subtraction here. pWatts is a covariance of two
    // mean-removed signals, so it already excludes the sensor's DC offset
    // (i_dc * mean(vCentered) = i_dc * 0) and any AC ripple uncorrelated with
    // the mains waveform. The old `fabsf(pWatts) - voltageRMS * floor` removed
    // a bias that was never present, and because the two trims disagreed the
    // board could show 0.62 A of current alongside 0 W of power.
    //
    // No-load deadband (see NO_LOAD_CURRENT_A / NO_LOAD_POWER_W in config.h):
    // below either threshold the channel reports P=0, S=0, PF=0 and integrates
    // nothing. fabs() rectifies noise, so without this an empty socket reads a
    // flickering 1-2 W that creeps into kWh and gives noise a "valid" PF that
    // re-rings a latched trip one cycle at a time. The displayed current keeps
    // its measured (floor-trimmed) value — only power, PF and energy blank.
    float rawP = fabsf(pWatts);
    bool noLoad = (filteredCurrentRMS[ch] < NO_LOAD_CURRENT_A) || (rawP < NO_LOAD_POWER_W);
    if (noLoad) {
      activePower[ch] = 0.0f;
      apparentPower[ch] = 0.0f;
      powerFactor[ch] = 0.0f;
      continue;
    }
    activePower[ch] = rawP;
    if (activePower[ch] < 0.0f) activePower[ch] = 0.0f;

    float vActual = (vAdcRMS / ADC_MAX_VALUE) * ADC_REFERENCE_V * voltageCal;
    float iActual = filteredCurrentRMS[ch];
    apparentPower[ch] = vActual * iActual;

    if (apparentPower[ch] > 0.001f) {
      powerFactor[ch] = activePower[ch] / apparentPower[ch];
      if (powerFactor[ch] > 1.0f) powerFactor[ch] = 1.0f;
    } else {
      powerFactor[ch] = 0.0f;
    }
  }
}

void PowerCalculator::setNoiseFloor(int ch, float val) {
  if (ch < 0 || ch >= NUM_CHANNELS) return;
  noiseFloor[ch] = val;
  filteredCurrentRMS[ch] = 0.0f;
  rmsInit[ch] = false;
}

void PowerCalculator::setLpfAlpha(int ch, float val) {
  if (ch < 0 || ch >= NUM_CHANNELS) return;
  if (val < 0.01f) val = 0.01f;
  if (val > 1.0f) val = 1.0f;
  lpfAlpha[ch] = val;
  rmsInit[ch] = false;
}

bool PowerCalculator::requestAutoZero(int ch) {
  if (ch < 0 || ch >= NUM_CHANNELS) return false;
  if (azActive && azChannel == ch) return false;
  for (int i = 0; i < azQueueLen; i++) {
    if (azQueue[i] == ch) return false;
  }
  if (azQueueLen >= NUM_CHANNELS) return false;
  azQueue[azQueueLen++] = ch;
  return true;
}

bool PowerCalculator::isAutoZeroBusy() const {
  return azActive || azQueueLen > 0;
}

int PowerCalculator::getAutoZeroQueue(int *out, int maxLen) const {
  int n = (azQueueLen < maxLen) ? azQueueLen : maxLen;
  for (int i = 0; i < n; i++) out[i] = azQueue[i];
  return azQueueLen;
}

bool PowerCalculator::autoZeroStart() {
  if (!azActive) {
    if (azQueueLen <= 0) return false;
    azChannel = azQueue[0];
    for (int i = 1; i < azQueueLen; i++) {
      azQueue[i - 1] = azQueue[i];
    }
    azQueueLen--;
    azBatchCount = 0;
    azActive = true;
    azLpfForced = true;
  }
  return true;
}

int PowerCalculator::autoZeroCapture(int n) {
  int captured = 0;
  while (captured < n && azBatchCount < azBatches) {
    azFloors[azBatchCount++] = runAutoZeroSingle(azChannel);
    captured++;
  }
  return captured;
}

float PowerCalculator::autoZeroFinish() {
  azLpfForced = false;
  for (int i = 0; i < NUM_CHANNELS; i++) {
    rmsInit[i] = false;
  }
  for (int i = 0; i < azBatches; i++) {
    for (int j = i + 1; j < azBatches; j++) {
      if (azFloors[j] < azFloors[i]) {
        float t = azFloors[i]; azFloors[i] = azFloors[j]; azFloors[j] = t;
      }
    }
  }
  float median = (azBatches % 2 == 1)
      ? azFloors[azBatches / 2]
      : (azFloors[azBatches / 2 - 1] + azFloors[azBatches / 2]) * 0.5f;
  azActive = false;
  azChannel = -1;
  azBatchCount = 0;
  return median;
}

float PowerCalculator::runAutoZeroSingle(int ch) {
  collectSamples();
  computeAll();
  return currentRMS[ch];
}

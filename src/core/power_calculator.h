#pragma once

#include "../config.h"

// Handles combined voltage+current sampling and computes power metrics.
// Uses interleaved sampling: at each sample index, reads voltage then all
// 6 current channels. This gives paired V-I samples for real power calculation.

class PowerCalculator {
public:
  void begin();

  // Performs one full sampling cycle and computes all power metrics.
  // Must be called repeatedly (typically from Core 1 sensor task).
  // deltaSeconds = time since last call for energy accumulation.
  void update(float deltaSeconds);

  // Accessors (thread-safe via shared data — caller manages semaphore)
  float getVoltageRMS() const       { return voltageRMS; }
  float getCurrentRMS(int ch) const { return currentRMS[ch]; }
  float getActivePower(int ch) const   { return activePower[ch]; }
  float getApparentPower(int ch) const { return apparentPower[ch]; }
  float getPowerFactor(int ch) const   { return powerFactor[ch]; }
  float getEnergyKWh(int ch) const     { return energyKWh[ch]; }
  void resetEnergy(int ch)             { if (ch >= 0 && ch < NUM_CHANNELS) energyKWh[ch] = 0.0f; }

  // Calibration factors (set from NVS)
  float voltageCal;
  float currentCal[NUM_CHANNELS];

private:
  // Raw sample buffers (paired V-I per sample index)
  float voltageSamples[RMS_SAMPLES];
  float currentSamples[NUM_CHANNELS][RMS_SAMPLES];

  // Results
  float voltageRMS;
  float currentRMS[NUM_CHANNELS];
  float activePower[NUM_CHANNELS];
  float apparentPower[NUM_CHANNELS];
  float powerFactor[NUM_CHANNELS];
  float energyKWh[NUM_CHANNELS];

  void collectSamples();
  void computeAll();
};

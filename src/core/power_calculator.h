#pragma once

#include "../config.h"

class PowerCalculator {
public:
  static const int AZ_BATCHES = 32;

  void begin();

  void update(float deltaSeconds);

  float getVoltageRMS() const       { return voltageRMS; }
  float getCurrentRMS(int ch) const { return filteredCurrentRMS[ch]; }
  float getActivePower(int ch) const   { return activePower[ch]; }
  float getApparentPower(int ch) const { return apparentPower[ch]; }
  float getPowerFactor(int ch) const   { return powerFactor[ch]; }
  float getEnergyKWh(int ch) const     { return energyKWh[ch]; }
  void resetEnergy(int ch)             { if (ch >= 0 && ch < NUM_CHANNELS) energyKWh[ch] = 0.0f; }
  void setEnergyKWh(int ch, float v)   { if (ch >= 0 && ch < NUM_CHANNELS) energyKWh[ch] = v; }

  float voltageCal;
  float currentCal[NUM_CHANNELS];

  float noiseFloor[NUM_CHANNELS];
  float lpfAlpha[NUM_CHANNELS];

  void setNoiseFloor(int ch, float val);
  void setLpfAlpha(int ch, float val);
  bool requestAutoZero(int ch);
  bool isAutoZeroBusy() const;
  bool isAutoZeroActive() const { return azActive; }
  int getAutoZeroChannel() const { return azActive ? azChannel : -1; }
  int getAutoZeroProgress() const { return azActive ? azBatchCount : 0; }
  int getAutoZeroQueue(int *out, int maxLen) const;
  bool autoZeroStart();
  int autoZeroCapture(int n);
  bool autoZeroDone() const { return azActive && azBatchCount >= AZ_BATCHES; }
  float autoZeroFinish();
  float runAutoZeroSingle(int ch);

  uint16_t rmsSamples;
  void setRmsSamples(uint16_t n) { if (n >= 100 && n <= MAX_RMS_SAMPLES) rmsSamples = n; }

private:
  float voltageSamples[MAX_RMS_SAMPLES];
  float currentSamples[NUM_CHANNELS][MAX_RMS_SAMPLES];

  float voltageRMS;
  float currentRMS[NUM_CHANNELS];
  float filteredCurrentRMS[NUM_CHANNELS];
  bool rmsInit[NUM_CHANNELS];
  float activePower[NUM_CHANNELS];
  float apparentPower[NUM_CHANNELS];
  float powerFactor[NUM_CHANNELS];
  float energyKWh[NUM_CHANNELS];

  int azQueue[NUM_CHANNELS];
  int azQueueLen;
  int azChannel;
  int azBatchCount;
  float azFloors[AZ_BATCHES];
  bool azActive;
  bool azLpfForced;

  void collectSamples();
  void computeAll();
};

#pragma once

#include <Arduino.h>

// ==============================
// Pin Assignments
// ==============================
#define PIN_CURRENT_CH1   2
#define PIN_CURRENT_CH2   16
#define PIN_CURRENT_CH3   4
#define PIN_CURRENT_CH4   5
#define PIN_CURRENT_CH5   6
#define PIN_CURRENT_CH6   7

#define PIN_VOLTAGE       1

#define PIN_RELAY_1       43
#define PIN_RELAY_2       44
#define PIN_RELAY_3       13
#define PIN_RELAY_4       12

#define PIN_RGB_LED       48

// ==============================
// Relay Configuration
// ==============================
// Set to LOW if relay module triggers on LOW signal (common), HIGH otherwise
#define RELAY_ACTIVE_STATE  LOW

// Channels 1-4 have relays; channels 5-6 are monitoring-only
#define RELAY_CHANNEL_COUNT  4

// ==============================
// ADC Configuration
// ==============================
#define ADC_RESOLUTION        12
#define ADC_MAX_VALUE         4095
#define ADC_REFERENCE_V        3.3f

// Mid-supply bias for AC-coupled signals
#define AC_BIAS_VOLTAGE       1.65f

// ADC sampling
#define ADC_READ_INTERVAL_US  40     // 25kHz sampling rate
#define RMS_SAMPLES           2000   // Samples per RMS calculation (~80ms @ 25kHz)
#define SAMPLES_PER_CYCLE      500   // ~500 samples per 50Hz cycle at 25kHz

// ==============================
// Firmware Version
// ==============================
#define FIRMWARE_VERSION "1.1.0"

// ==============================
// Default Calibration Constants
// ==============================
// SCT-013-100: 100A RMS → 1V RMS output
// Calibration = (ADC_MAX_VALUE / ADC_REFERENCE_V) * (rated_current / rated_voltage_output) / sqrt(2)
// Simplified: adjust so that at 1V RMS input (100A), reading = 100.0A
#define DEFAULT_CURRENT_CALIBRATION   100.0f

// ZMPT101B: typical output varies by module (potentiometer adjustable)
#define DEFAULT_VOLTAGE_CALIBRATION   260.0f

// ==============================
// Channel Configuration
// ==============================
#define NUM_CHANNELS         6
#define NUM_RELAYS           4
#define MAX_CHANNEL_NAME_LEN 24

// Default limits
#define DEFAULT_CURRENT_LIMIT_A   16.0f
#define DEFAULT_POWER_LIMIT_W     3500.0f
#define WARNING_THRESHOLD_PCT     90

// ==============================
// Timing Constants (milliseconds)
// ==============================
#define ENERGY_UPDATE_INTERVAL_MS  1000
#define WS_UPDATE_INTERVAL_MS      500
#define SENSOR_CYCLE_INTERVAL_MS   100

// ==============================
// Event Log
// ==============================
#define EVENT_LOG_SIZE    50
#define EVENT_MSG_LEN     64

// ==============================
// WiFi
// ==============================
#define WIFI_RETRY_INTERVAL_MS  10000
#define WIFI_MAX_RETRIES        5
#define AP_FALLBACK_TIMEOUT_MS  30000

// ==============================
// Channel Status Enum
// ==============================
enum ChannelStatus : uint8_t {
  STATUS_OK = 0,
  STATUS_WARNING = 1,
  STATUS_TRIPPED = 2,
  STATUS_DISABLED = 3
};

// ==============================
// Event Structure
// ==============================
struct Event {
  uint32_t timestamp;
  uint8_t channel;
  ChannelStatus status;
  float value;
  char message[EVENT_MSG_LEN];
};

// ==============================
// Channel Data Structure
// ==============================
struct ChannelData {
  char name[MAX_CHANNEL_NAME_LEN];
  float currentRMS;
  float activePower;       // W
  float apparentPower;     // VA
  float powerFactor;
  float energyKWh;
  float currentLimit;
  float powerLimit;
  ChannelStatus status;
  bool relayOn;
};

// ==============================
// Shared Data (between cores)
// ==============================
struct SystemData {
  ChannelData channels[NUM_CHANNELS];
  float voltageRMS;
  float voltageCalibration;
  float currentCalibration[NUM_CHANNELS];
  uint32_t uptime;

  bool wifiConnected;
  int8_t wifiRSSI;
  bool apMode;

  Event events[EVENT_LOG_SIZE];
  uint8_t eventCount;

  // OTA state
  bool otaInProgress;
  uint8_t otaProgress;
};

// Current sensor pins array
static const uint8_t CURRENT_PINS[NUM_CHANNELS] = {
  PIN_CURRENT_CH1, PIN_CURRENT_CH2, PIN_CURRENT_CH3,
  PIN_CURRENT_CH4, PIN_CURRENT_CH5, PIN_CURRENT_CH6
};

// Relay pins array
static const uint8_t RELAY_PINS[NUM_RELAYS] = {
  PIN_RELAY_1, PIN_RELAY_2, PIN_RELAY_3, PIN_RELAY_4
};

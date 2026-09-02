#pragma once

#include <Arduino.h>

// ==============================
// Pin Assignments
// ==============================
#define PIN_CURRENT_CH1   7
#define PIN_CURRENT_CH2   5
#define PIN_CURRENT_CH3   6
#define PIN_CURRENT_CH4   8
#define PIN_CURRENT_CH5   4
#define PIN_CURRENT_CH6   2

#define PIN_VOLTAGE       1

#define PIN_BUZZER        13

#define PIN_RGB_LED       48

// ==============================
// ntfy.sh Push Notification
// ==============================
#define NTFY_HOST  "ntfy.sh"
#define NTFY_PORT  443

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
#define MAX_RMS_SAMPLES       2000   // Max buffer size for runtime-tunable RMS samples
#define SAMPLES_PER_CYCLE      500   // ~500 samples per 50Hz cycle at 25kHz

// ==============================
// Firmware Version
// ==============================
#define FIRMWARE_VERSION "2.0.0"

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
#define MAX_CHANNEL_NAME_LEN 24

// Default monthly kWh limit
#define DEFAULT_MONTHLY_KWH_LIMIT 48.0f

// Monthly billing reset day (1-28). Counters zero at 00:00 UTC on this day each month.
// Billing period = 25th → 24th next month. Use 1 for calendar-month reset.
#define MONTHLY_RESET_DAY 25

// Default per-channel LPF alpha (EMA on post-RMS estimate; 1.0 = no filtering)
#define DEFAULT_LPF_ALPHA 0.2f

// ==============================
// Auto-Recovery (limit trip)
// ==============================
#define AUTO_RECOVER_PF 0.1f

// ==============================
// Timing Constants (milliseconds)
// ==============================
#define ENERGY_UPDATE_INTERVAL_MS  1000
#define WS_UPDATE_INTERVAL_MS      150
#define SENSOR_CYCLE_INTERVAL_MS   80

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
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define WIFI_MAX_BOOT_FAILURES  10

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
  float monthlyKwhLimit;
  ChannelStatus status;
};

// ==============================
// Shared Data (between cores)
// ==============================
struct SystemData {
  ChannelData channels[NUM_CHANNELS];
  float voltageRMS;
  float voltageCalibration;
  float currentCalibration[NUM_CHANNELS];
  uint16_t rmsSamples;
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

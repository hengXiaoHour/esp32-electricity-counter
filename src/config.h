#pragma once

#include <Arduino.h>

// ==============================
// Pin Assignments
// ==============================
// Two build targets share this file. The classic ESP32 only exposes 6
// WiFi-safe ADC1 pins (GPIO 32/33/34/35/36/39); ADC2 pins (GPIO 0/2/4/12-15/
// 25/26/27) return garbage while the AP radio is on, so all analog inputs
// must live on ADC1. That is why both variants run 5 current channels +
// 1 voltage input.
//
// ESP32 classic boot-strapping pins avoided below: 0, 2, 12, 15 (also flash
// 6-11, UART 1/3, PSRAM 16/17 on WROVER). GPIO 34/35/36/39 are input-only.

#if defined(CONFIG_IDF_TARGET_ESP32S3)
// ESP32-S3: ADC1 = GPIO 1-10. RGB LED on GPIO 48 (devkit built-in).
#define PIN_CURRENT_CH1   7
#define PIN_CURRENT_CH2   5
#define PIN_CURRENT_CH3   6
#define PIN_CURRENT_CH4   8
#define PIN_CURRENT_CH5   4

#define PIN_VOLTAGE       1

#define PIN_BUZZER        13

#define PIN_RGB_LED       48

#else
// ESP32 (classic): GPIO 36/39/34/35/32 for the five CTs, GPIO 33 for ZMPT101B.
// Buzzer on GPIO 13; status LED on GPIO 2, the devkit's built-in blue LED
// (kept out of the ADC map on purpose: GPIO 2 is ADC2, which is unusable while
// the WiFi radio is on - fine for an LED, never for a sensor).
#define PIN_CURRENT_CH1   36
#define PIN_CURRENT_CH2   39
#define PIN_CURRENT_CH3   34
#define PIN_CURRENT_CH4   35
#define PIN_CURRENT_CH5   32

#define PIN_VOLTAGE       33

#define PIN_BUZZER        13

#define PIN_RGB_LED       2

#endif

// ==============================
// ADC Configuration
// ==============================
#define ADC_RESOLUTION        12
#define ADC_MAX_VALUE         4095
#define ADC_REFERENCE_V        3.3f

// NOTE: there is deliberately no AC_BIAS_VOLTAGE constant here any more. Both
// the CT and ZMPT outputs are biased to mid-supply, but the RMS math
// mean-removes that bias per sample rather than subtracting a hard-coded 1.65 V
// — a fixed constant would be wrong the moment the bias moved, and it was
// never read by anything.

// ADC sampling
#define ADC_READ_INTERVAL_US  40     // 25kHz sampling rate
#define MAX_RMS_SAMPLES       2000   // Max buffer size for runtime-tunable RMS samples

// ==============================
// Firmware Version
// ==============================
// 3.1.0 = local-midnight billing (UTC+7), reboot-safe NVS (no more
// reboot wipes), forward-only rollover with unset-marker anchoring,
// execute-once cloud downlink, guarded test_force_rollover CLI.
#define FIRMWARE_VERSION "3.2.14"

// ==============================
// Cloud OTA (serial-first, cloud-ota proven pattern)
// ==============================
// The board polls this version.json (same shape the cloud-ota rig uses)
// and flashes the per-chip release asset. Serial verbs only for now:
// `version` prints the compiled stamp, `update` checks + stages + reboots,
// `ota <url>` / `ota status` keep their existing direct-link shape. No web
// button yet - the dashboard Firmware panel still sends `ota <url>`.
//
// USE_INSECURE is deliberate, not lazy: the validated-TLS downloader
// (3.2.0-3.2.12) never completed a handshake on this board (34 KB largest
// block), while the cloud-ota rig's setInsecure + HTTPUpdate flashes
// reliably. Trust comes from the /cmd downlink only the admin Gmail can
// write + Update.end() image validation, same as the cloud-ota rig.
#define OTA_VERSION_URL "https://raw.githubusercontent.com/hengXiaoHour/esp32-electricity-counter/main/version.json"
#define OTA_USE_INSECURE true

// Auto-check (cloud-ota parity, notify-only): the network tick polls
// version.json every OTA_CHECK_INTERVAL_MS and prints ONE serial line per
// new release. Already-on-latest and failed checks stay silent - no spam.
// It never stages or reboots by itself; `update` stays the trigger.
#define OTA_CHECK_INTERVAL_MS 3600000

// ==============================
// Station WiFi (STA) - the DEFAULT path. The board joins this home network on
// boot; the AP below is fallback-only and stays OFF unless the home link fails.
// `setwifi <ssid> <pass>` (serial or dashboard) stores the real thing in NVS and
// takes precedence over this default. To hard-wire a network instead, put it
// here.
#define STA_SSID_PLACEHOLDER "YOUR_HOME_SSID"
#define STA_SSID_DEFAULT STA_SSID_PLACEHOLDER
#define STA_PASS_DEFAULT "YOUR_HOME_PASSWORD"

// How long to wait for the home network at boot before giving up and starting
// the fallback AP. The board is unreachable during this window, so it is short
// on purpose - the STA retry continues in the background once the AP is up.
#define STA_CONNECT_TIMEOUT_MS 10000

// ==============================
// Cloud (herd login: email/password -> ID token)
// ==============================
// The database host + Web API key are PUBLIC values (the key only names the
// project - it ships inside every web app; the password does the securing),
// so they live here as defaults rather than NVS settings. The ACCOUNT
// (email + password) stays in NVS via `setcloud`, like the WiFi credentials.
#define CLOUD_API_KEY_DEFAULT "AIzaSyA1BCYnxBc9q_ONa58TTkGimlGPn0wyvj0"
#define CLOUD_DB_HOST_DEFAULT "esp32-electricity-counter-default-rtdb.firebaseio.com"

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
// Access Point
// ==============================
// The board IS the network, so these are its factory identity - the name and
// password it broadcasts on a fresh board, or after `reset_ap`. Both are
// overridable at runtime and stored in NVS under ap_ssid / ap_pass (see
// NVSManager); these constants are the fallback when flash holds nothing usable,
// which is also what keeps a corrupt value from taking the radio down.
//
// They live here rather than in wifi_manager.h because NVSManager needs them as
// its default and it has no business including <WiFi.h> for two string literals.
// (Previously both the README and check_docs.py pointed at wifi_manager.h.)
constexpr const char *AP_SSID_DEFAULT = "ESP32-Elec-Counter";
constexpr const char *AP_PASS_DEFAULT = "configure123";

// ==============================
// Channel Configuration
// ==============================
#define NUM_CHANNELS         5
#define MAX_CHANNEL_NAME_LEN 24

// Default monthly kWh limit
#define DEFAULT_MONTHLY_KWH_LIMIT 48.0f

// Board wall-clock timezone: UTC+7 (Asia/Phnom_Penh). Cambodia has no DST,
// so a fixed offset is exact. NTP and the browser lend still speak epoch, so
// only the localtime_r() calendar moves — billing boundaries land at LOCAL
// midnight instead of 00:00 UTC.
#define TIMEZONE_OFFSET_SECONDS (7 * 3600)

// Monthly billing reset day (1-28). Counters zero at 00:00 local time (UTC+7) on this day each month.
// Billing period = 25th → 24th next month. Use 1 for calendar-month reset.
// This is only the FACTORY default: the user can change it at runtime via
// `reset_day <1-28>` (serial / dashboard console / cloud console) or the
// About panel, and it persists in NVS under reset_day.
#define MONTHLY_RESET_DAY 25

// Default per-channel LPF alpha (EMA on post-RMS estimate; 1.0 = no filtering)
#define DEFAULT_LPF_ALPHA 1.0f

// ==============================
// Auto-Recovery (limit trip)
// ==============================
#define AUTO_RECOVER_PF 0.2f

// ==============================
// Timing Constants (milliseconds)
// ==============================
#define WS_UPDATE_INTERVAL_MS      150
#define SENSOR_CYCLE_INTERVAL_MS   80

// ==============================
// Event Log
// ==============================
#define EVENT_LOG_SIZE    50
#define EVENT_MSG_LEN     64

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
  // Dotted-decimal interface addresses, refreshed every sensor cycle in
  // updateSharedData(). Empty string = that interface is down. The dashboard
  // prints these (STA panel + AP panel + Connection Board row) so the user
  // can see the address to open in BOTH modes without guessing.
  char staIp[16];
  char apIp[16];

  float mcuTempC;    // on-die temperature, °C (NAN when the chip has no sensor)
  bool ecoMode;      // true while nobody is watching (modem-sleep + quiet radio)

  // Remote-monitoring status for the dashboard. No secret here by construction:
  // the token lives only in NVS and inside the TLS tunnel - cloudDev is the
  // MAC id (also the database path), cloudAgeS is -1 when nothing ever landed.
  bool cloudEnabled;
  bool cloudOk;
  int32_t cloudAgeS;
  char cloudDev[13];

  Event events[EVENT_LOG_SIZE];
  uint8_t eventCount;

  // OTA state
  bool otaInProgress;
  uint8_t otaProgress;
};

// Current sensor pins array
static const uint8_t CURRENT_PINS[NUM_CHANNELS] = {
  PIN_CURRENT_CH1, PIN_CURRENT_CH2, PIN_CURRENT_CH3,
  PIN_CURRENT_CH4, PIN_CURRENT_CH5
};

#pragma once

#include <Arduino.h>

// Stable, chip-unique device ID derived from the ESP32 efuse MAC address
// (lower 3 bytes -> 6 hex chars). Survives reflashes; differs per board,
// so multiple boards can share one Firebase RTDB without clobbering each
// other's /latest, /commands and /ota nodes.
String deviceId();
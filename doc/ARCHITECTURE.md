# Project Architecture — ESP32-S3 6-Channel AC Electricity Counter

## 🔒 ARCHITECTURE LOCK

This file defines the **canonical project structure**. All AI agents MUST read this file before creating or modifying any files. The structure below is **LOCKED** — do not create files or directories outside this layout without explicit user approval.

Machine-readable lock: `./.architecture.lock.json`

## Directory Tree

```
esp32-electricity-counter/
├── doc/                              # Documentation
│   ├── ARCHITECTURE.md               # THIS FILE — project structure (locked)
│   ├── .architecture.lock.json       # Machine-readable lock (do not edit)
│   ├── esp32s3-electricity-counter-prompt.md  # Original build prompt
│   └── opencode_agent/               # Agent context (auto-managed)
│       ├── AGENTS.md
│       ├── lessons.md
│       └── memories.json
│
├── src/                              # Application source code
│   ├── main.ino                      # Arduino sketch entry point
│   ├── config.h                      # Pin assignments and #define constants
│   ├── sensor/                       # ADC sampling and RMS calculation
│   │   ├── current_sensor.h
│   │   ├── current_sensor.cpp
│   │   ├── voltage_sensor.h
│   │   └── voltage_sensor.cpp
│   ├── core/                         # Business logic
│   │   ├── power_calculator.h        # Real power, apparent power, PF, kWh
│   │   ├── power_calculator.cpp
│   │   ├── limit_manager.h           # Per-channel limits, warning/trip logic
│   │   ├── limit_manager.cpp
│   │   ├── relay_controller.h        # Relay switching
│   │   └── relay_controller.cpp
│   ├── network/                      # Networking (Core 0 tasks)
│   │   ├── wifi_manager.h
│   │   ├── wifi_manager.cpp
│   │   ├── websocket_server.h
│   │   ├── websocket_server.cpp
│   │   ├── ota_handler.h
│   │   └── ota_handler.cpp
│   ├── ui/                           # Status display
│   │   ├── status_led.h              # WS2812 RGB LED (R/G swap handled)
│   │   └── status_led.cpp
│   └── utils/                        # Shared utilities
│       ├── nvs_manager.h             # NVS/Preferences storage
│       └── nvs_manager.cpp
│
├── data/                             # LittleFS filesystem (web dashboard)
│   ├── index.html                    # Dashboard HTML
│   ├── style.css                     # Dashboard styles
│   └── script.js                     # Dashboard WebSocket client
│
├── scripts/                          # Utility scripts
│   └── deploy.sh
│
├── .gitignore
└── README.md                         # Setup instructions
```

## Conventions

| Aspect | Rule |
|---|---|
| **C++ files** | `snake_case.h` / `snake_case.cpp` |
| **Arduino sketch** | `main.ino` at `src/` root |
| **Header guards** | `#pragma once` |
| **Documentation** | `doc/` directory only |
| **Web files** | `data/` directory for LittleFS |
| **Naming** | `snake_case` for files and functions |
| **Indentation** | 2 spaces |
| **Platform** | Arduino IDE / Arduino framework (not ESP-IDF, not PlatformIO) |
| **Target** | ESP32-S3 |

## Dual-Core Architecture

- **Core 0**: Networking + WebSocket + dashboard + OTA (responsiveness priority)
- **Core 1**: Real-time ADC sampling + RMS math + limit checking + relay control (deterministic priority)
- Shared data protected by FreeRTOS `SemaphoreHandle_t`

## Lock Enforcement

1. **Before creating any file**, read this `ARCHITECTURE.md` and `.architecture.lock.json`
2. If the new file path fits the tree above — proceed
3. If a directory doesn't exist yet but fits logically — ask the user
4. If the file doesn't fit the structure at all — BLOCKED, explain why to the user

## Pin Mapping

| Component | Pins | Notes |
|---|---|---|
| CT sensors (6ch) | GPIO2, 16, 4, 5, 6, 7 | SCT-013-100, analog input |
| Voltage sensor | GPIO1 | ZMPT101B, analog input |
| Relays (4ch) | GPIO43, 44, 13, 12 | Active-LOW (configurable) |
| RGB LED | GPIO48 | WS2812, R/G channels swapped |

Channels 1–4 are relay-controllable; channels 5–6 are monitoring-only.

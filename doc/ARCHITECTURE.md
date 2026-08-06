# Project Architecture — ESP32-S3 6-Channel AC Electricity Counter

## 🔒 ARCHITECTURE LOCK

This file defines the **canonical project structure**. All AI agents MUST read this file before creating or modifying any files. The structure below is **LOCKED** — do not create files or directories outside this layout without explicit user approval.

Machine-readable lock: `./.architecture.lock.json`

## Directory Tree

```
esp32-electricity-counter/
├── esp32-electricity-counter.ino    # Arduino sketch entry point (setup + loop + FreeRTOS tasks)
├── .gitignore
├── README.md                        # Setup instructions
│
├── doc/                             # Documentation
│   ├── ARCHITECTURE.md              # THIS FILE — project structure (locked)
│   ├── .architecture.lock.json      # Machine-readable lock (do not edit)
│   ├── esp32s3-electricity-counter-prompt.md  # Original build prompt
│   └── opencode_agent/              # Agent context (auto-managed)
│       ├── AGENTS.md
│       ├── lessons.md
│       └── memories.json
│
├── src/                             # Application source code
│   ├── config.h                     # Pin assignments, #define constants, shared data structs
│   ├── sensor/
│   │   ├── current_sensor.h         # 6-ch ADC sampling + RMS calculation
│   │   └── current_sensor.cpp
│   │   ├── voltage_sensor.h         # Voltage ADC sampling + RMS calculation
│   │   └── voltage_sensor.cpp
│   ├── core/
│   │   ├── power_calculator.h       # Combined V-I sampling, real/apparent power, PF, kWh
│   │   ├── power_calculator.cpp
│   │   ├── limit_manager.h          # 100%-limit trip state machine, event log
│   │   └── limit_manager.cpp
│   ├── network/
│   │   ├── wifi_manager.h           # WiFi connect + fallback AP + captive portal
│   │   ├── wifi_manager.cpp
│   │   ├── websocket_server.h       # AsyncWebSocket, JSON broadcast, command parsing
│   │   ├── websocket_server.cpp
│   │   ├── ntfy_notifier.h          # ntfy.sh push notifications (WiFiClientSecure POST)
│   │   ├── ntfy_notifier.cpp
│   │   ├── ota_handler.h            # ArduinoOTA setup
│   │   └── ota_handler.cpp
│   ├── ui/
│   │   ├── status_led.h             # WS2812 with R/G channel swap
│   │   ├── status_led.cpp
│   │   ├── buzzer.h                 # Non-blocking active-buzzer beep pattern driver
│   │   └── buzzer.cpp
│   └── utils/
│       ├── nvs_manager.h            # Preferences wrapper for channel configs, WiFi, cal
│       └── nvs_manager.cpp
│
├── data/                            # Web dashboard files (hosted on PC/phone, NOT on ESP32)
│   ├── index.html                   # Dashboard HTML — includes connection panel for IP input
│   ├── style.css                    # Dashboard styles — dark/red theme
│   └── script.js                    # WebSocket client — connects to user-specified ESP32 IP
│
├── .workflow/                        # AI agent workflow state
│   ├── RESEARCH.md
│   ├── PLAN.md
│   └── VERIFICATION.log
│
└── scripts/                         # Utility scripts
    └── deploy.sh
```

## Conventions

| Aspect | Rule |
|---|---|
| **Arduino sketch** | `.ino` at project root, named `esp32-electricity-counter.ino` |
| **C++ sources** | `snake_case.h` / `snake_case.cpp` in `src/` |
| **Header guards** | `#pragma once` |
| **Documentation** | `doc/` directory only |
| **Web files** | `data/` directory for LittleFS |
| **Naming** | `snake_case` for files, `PascalCase` for classes |
| **Indentation** | 2 spaces |
| **Platform** | Arduino IDE / Arduino framework (not ESP-IDF, not PlatformIO) |
| **Target** | ESP32-S3 |

## Dual-Core Architecture

- **Core 0** (priority 1): Networking + WebSocket + OTA
- **Core 1** (priority 2): ADC sampling + power math + limit checking + buzzer alerts
- Shared `SystemData` struct protected by FreeRTOS `SemaphoreHandle_t`

## Lock Enforcement

1. **Before creating any file**, read this `ARCHITECTURE.md` and `.architecture.lock.json`
2. If the new file path fits the tree above — proceed
3. If a directory doesn't exist yet but fits logically — ask the user
4. If the file doesn't fit the structure at all — BLOCKED, explain why to the user

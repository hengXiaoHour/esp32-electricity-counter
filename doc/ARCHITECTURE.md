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
├── firebase.json                    # Firebase Hosting config (public = frontend/, SPA rewrite)
├── .firebaserc                      # Firebase project alias (default = esp32-electricity-counter)
├── database.rules.json              # RTDB security rules (latest/ + commands/)
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
│   │   ├── websocket_server.h       # AsyncWebSocket, JSON broadcast, delegates commands
│   │   ├── websocket_server.cpp
│   │   ├── command_processor.h      # Shared command parser (WS + Firebase): set_name, cal, ...
│   │   ├── command_processor.cpp
│   │   ├── firebase_bridge.h        # Firebase RTDB bridge: /latest push + /commands poll
│   │   ├── firebase_bridge.cpp      # buildSystemJson shared JSON builder + FirebaseBridge
│   │   ├── firebase_config.h        # REAL Firebase credentials (gitignored, placeholders now)
│   │   ├── firebase_config.example.h# Committed credential template
│   │   ├── ntfy_notifier.h          # ntfy.sh push notifications (WiFiClientSecure POST)
│   │   ├── ntfy_notifier.cpp
│   │   ├── ota_handler.h            # ArduinoOTA (local WiFi OTA from Arduino IDE)
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
├── frontend/                        # Web dashboard — PWA hosted on Firebase Hosting (NOT on ESP32)
│   ├── index.html                   # Dashboard HTML — Cloud / Local / Demo connect panel
│   ├── style.css                    # Dashboard styles — dark/red theme
│   ├── script.js                    # WebSocket + Firebase RTDB client, 3 connection modes
│   ├── config.js                    # Firebase web config (gitignored; copy from example)
│   ├── config.example.js            # Committed config template
│   ├── manifest.json                # PWA manifest
│   ├── sw.js                        # Service worker (network-first + cache fallback)
│   └── icons/                       # PWA icons
│
├── .workflow/                        # AI agent workflow state
│   ├── RESEARCH.md
│   ├── PLAN.md
│   └── VERIFICATION.log
│
└── tools/                          # Utility scripts
     └── hosting_deploy.py
```

## Conventions

| Aspect | Rule |
|---|---|
| **Arduino sketch** | `.ino` at project root, named `esp32-electricity-counter.ino` |
| **C++ sources** | `snake_case.h` / `snake_case.cpp` in `src/` |
| **Header guards** | `#pragma once` |
| **Documentation** | `doc/` directory only |
| **Web files** | `frontend/` directory (PWA hosted on Firebase Hosting) |
| **Naming** | `snake_case` for files, `PascalCase` for classes |
| **Indentation** | 2 spaces |
| **Platform** | Arduino IDE / Arduino framework (not ESP-IDF, not PlatformIO) |
| **Target** | ESP32-S3 |

## Dual-Core Architecture

- **Core 0** (priority 1): Networking + WebSocket + Firebase RTDB bridge + OTA
- **Core 1** (priority 2): ADC sampling + power math + limit checking + buzzer alerts
- Shared `SystemData` struct protected by FreeRTOS `SemaphoreHandle_t`

## Cloud Bridge (Firebase RTDB)

- Device (service-account JWT auth) writes its full snapshot to `/latest` every 1 s.
- Dashboard (any browser) reads `/latest` and pushes commands to `/commands`.
- Device polls `/commands` every 1 s, executes each queued command via the shared `processCommand()`, and deletes it.
- Same JSON schema (`buildSystemJson`) is used for both WebSocket broadcasts and the `/latest` RTDB snapshot.
- **Two dedicated FirebaseData connections (`fbdo`, `fbCmd`)** — one TLS session each. A failing/empty GET on a shared connection tears down that TLS session and forces a ~1.3-1.9 s handshake on every push, so empty reads must never share the publish connection. The `/commands` poll treats a missing (`null`) node as normal (no teardown).

## Lock Enforcement

1. **Before creating any file**, read this `ARCHITECTURE.md` and `.architecture.lock.json`
2. If the new file path fits the tree above — proceed
3. If a directory doesn't exist yet but fits logically — ask the user
4. If the file doesn't fit the structure at all — BLOCKED, explain why to the user

# Project Architecture — ESP32 Electricity Counter

## 🔒 ARCHITECTURE LOCK

This file defines the **canonical project structure**. All AI agents MUST read this file before creating or modifying any files. The structure below is **LOCKED** — do not create files or directories outside this layout without explicit user approval.

Machine-readable lock: `./.architecture.lock.json`

## Directory Tree

```
esp32-electricity-counter/
├── doc/                          # Documentation
│   ├── ARCHITECTURE.md           # THIS FILE — project structure (locked)
│   ├── .architecture.lock.json   # Machine-readable lock (do not edit)
│   └── opencode_agent/           # Agent context (auto-managed)
│       ├── AGENTS.md
│       ├── lessons.md
│       └── memories.json
│
├── src/                          # Application source code
│   ├── main.py                   # Entry point
│   ├── config.py                 # Configuration
│   ├── hardware/                 # Hardware abstraction layer
│   │   ├── __init__.py
│   │   ├── sensor.py             # Current/voltage sensor interface
│   │   ├── display.py            # Display driver
│   │   └── wifi.py               # WiFi connectivity
│   ├── core/                     # Core business logic
│   │   ├── __init__.py
│   │   ├── counter.py            # Electricity counting logic
│   │   ├── calculator.py         # Consumption calculations
│   │   └── storage.py            # Data persistence
│   ├── api/                      # API layer
│   │   ├── __init__.py
│   │   └── server.py             # REST/Web server
│   └── utils/                    # Shared utilities
│       ├── __init__.py
│       ├── logger.py
│       └── helpers.py
│
├── tests/                        # Test suite
│   ├── __init__.py
│   ├── test_counter.py
│   ├── test_calculator.py
│   └── conftest.py               # Shared fixtures
│
├── config/                       # Configuration files
│   └── settings.yaml
│
├── data/                         # Runtime data (SPIFFS/LittleFS)
│
├── scripts/                      # Utility scripts
│   └── deploy.sh
│
├── requirements.txt              # Python dependencies
├── pyproject.toml                # Project config (lint, test, build)
├── .gitignore
└── README.md
```

## Conventions

| Aspect | Rule |
|---|---|
| **Python files** | `snake_case.py` |
| **Test files** | `test_<module>.py` in `tests/` |
| **Documentation** | `doc/` directory only |
| **Imports** | Absolute imports preferred |
| **Types** | Type hints required on all functions |
| **Tests** | pytest with fixtures |

## Lock Enforcement

1. **Before creating any file**, read this `ARCHITECTURE.md` and `.architecture.lock.json`
2. If the new file path fits the tree above — proceed
3. If a directory doesn't exist yet but fits logically — ask the user
4. If the file doesn't fit the structure at all — BLOCKED, explain why to the user

## Notes

- `src/hardware/` contains platform-specific hardware code (ESP32 GPIO, ADC, I2C)
- `src/core/` contains pure business logic with hardware abstraction
- `src/api/` handles external communication (HTTP, MQTT, etc.)
- `config/` contains static configuration, not secrets (use env vars or .env for secrets)

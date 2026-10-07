# ESP32 5-Channel AC Electricity Counter

A standalone box that watches 5 AC circuits — current, voltage, power, power
factor, monthly kWh per channel — on a live dashboard served by the board
itself. No cloud account, no internet needed: the ESP32 is the WiFi network
and the web server. Each channel has a monthly limit; on trip the buzzer
rings and the dashboard flags it. Counters survive power loss in flash.

Join `ESP32-Elec-Counter` (password `configure123`), open
`http://192.168.4.1/`, done. With home WiFi set, the board joins it and the
fallback AP stays off; an opt-in Firebase mirror pushes readings to
`/devices/<MAC>/latest` every second.

## Hardware

| Part | Pins |
|---|---|
| 5× CT sensors (SCT-013-100) | GPIO 7, 5, 6, 8, 4 |
| Voltage sensor (ZMPT101B) | GPIO 1 |
| Active buzzer | GPIO 13 |
| Status LED (plain / WS2812 RGB) | GPIO 48 |

Classic ESP32 uses an ADC1-only pin map; defaults live in `src/config.h`.

## Build & flash

`./scripts/build.sh` is the only supported entry point — it re-embeds the
dashboard and checks the AsyncTCP patches:

```bash
./scripts/build.sh --version X.Y.Z --output-dir /tmp/build          # ESP32-S3
./scripts/build.sh --classic --output-dir /tmp/build-classic        # ESP32 classic
```

Classic needs `esp32:esp32:esp32:PartitionScheme=min_spiffs` (the default
slot overflows at 103%). If the build complains about AsyncTCP, run
`scripts/patch_async_tcp.py`, then rebuild. Flash app-only, so counters,
PIN and credentials survive:

```bash
esptool --port /dev/ttyACM0 --chip esp32s3 --baud 460800 \
        write-flash 0x10000 /tmp/build/esp32-electricity-counter.ino.bin
```

Prebuilt `.bin` files ride each GitHub release.

### Updating the firmware

Local only: USB as above, or ArduinoOTA on the LAN (network port
`esp32-elec-counter`). The Firmware panel shows the running version.

## Use

- Rename the network with `set_ap` / `reset_ap` (serial or Settings,
  form is `set_ap <name> <pass>`). Changing the fallback AP name and password
  reboots the board; the AP only appears when home WiFi fails. Defaults `ESP32-Elec-Counter` /
  `configure123`.
- Home WiFi: `setwifi <ssid> <pass>` / `clearwifi`. Cloud mirror: `setcloud
  <host> <email> <pass>` / `clearcloud` (rules in `database.rules.json`).
- Anything that changes the board needs the admin PIN (default `1234`).
- Serial console (`help` for the list): `status`, `wifi`, `cal`, `inject`,
  `reboot`, …

## Limits

- No access from outside your own WiFi
- No true PWA install on desktop browsers
- The board never sleeps

Full version history: `gh release list`.

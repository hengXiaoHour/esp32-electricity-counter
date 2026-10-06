# ESP32 / ESP32-S3 5-Channel AC Electricity Counter

## 1. What is it?

A standalone box that watches 5 AC circuits at once — current, voltage, power,
power factor and monthly kWh per channel — and shows it all on a live web
dashboard served by the board itself. No cloud account, no router, no internet:
the ESP32 **is** the WiFi network and the web server.

Join `ESP32-Elec-Counter` (password `configure123`), open
`http://192.168.4.1/`, done.

Optional: give it your home WiFi plus a realtime-database account (email +
password) and it mirrors readings to
`/devices/<MAC>/latest` every second for checking from anywhere. The password
never leaves the board except inside that connection; the dashboard shows
push health but never the password.

## 2. Why?

- Know which circuit actually eats the power bill — per-channel W and kWh,
  not one meter for the whole house.
- Get warned before the bill gets big — each channel has a monthly limit; on
  trip the buzzer rings and the channel is flagged.
- Own your data — readings stay on your LAN by default, counters survive power loss in
  flash, browser works offline as an installed app. Cloud mirror is opt-in, push-only.
- No monthly subscription, no phone-home telemetry, no app store.

## 3. How?

**Hardware**

| Component | Pins |
|---|---|
| CT current sensors ×5 (SCT-013-100) | GPIO 7, 5, 6, 8, 4 |
| Voltage sensor (ZMPT101B) | GPIO 1 |
| Active buzzer | GPIO 13 |
| Status LED (plain / WS2812 RGB, CLI-selectable) | GPIO 48 |

**Firmware** — Arduino core for ESP32 (3.3.x), C++17, FreeRTOS with two cores:
a sensor task samples all ADC channels every 80 ms, computes true-RMS, power and
energy with an EMA low-pass per channel, and persists counters to NVS every 5 s;
a network task serves the dashboard and broadcasts snapshots over WebSocket at
~7 Hz. AsyncTCP is patched for lwIP core-locking; see `doc/opencode_agent/`.

**Dashboard** — a PWA written in plain HTML/CSS/JS (no framework, no build
step), embedded in flash as PROGMEM via `scripts/embed_web.py`. Same-origin
WebSocket means live updates with zero config, and the service worker makes it
installable on a phone.

**Security model** — viewing is open; anything that changes the board
(names, limits, calibration, counters, reboot, PIN) needs the admin PIN
(default `1234`, change it in Settings → Admin PIN). It is checked **on the
ESP32**, not in the browser.

Heads-up: when the fallback AP is up it has no upstream internet, so phones show
"no internet" when joined — expected. Forgot the AP password? `reset_ap` over
the serial console restores the factory network.

By default the board joins your home WiFi and the AP stays **OFF** (see **Home
Network** below) — the AP only appears when the home link fails.

## Build & flash

```bash
# ESP32-S3:
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=no_fs,CDCOnBoot=cdc .
scripts/patch_async_tcp.py --apply   # if AsyncTCP lacks the lwIP core-lock patches
esptool --port /dev/ttyACM0 --chip esp32s3 write-flash 0x10000 <sketch>.ino.bin

# ESP32 (classic):
# min_spiffs, not default: the TLS stack for cloud push put the firmware at
# 103% of the default 1.2 MB app slot. min_spiffs gives 1.9 MB AND keeps OTA
# (two OTA slots + otadata); NVS stays at 0x9000, so settings survive the
# switch - but the partition table itself must be flashed once (0x8000).
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs .
esptool --port /dev/ttyUSB0 --chip esp32 write-flash 0x10000 <sketch>.ino.bin
```

Both targets run 5 current channels + 1 voltage input, because the classic
ESP32 only exposes 6 WiFi-safe ADC1 pins. Default pins live in `src/config.h`
and are selected automatically by the compile target.

Rename/recover the network over serial: `set_ap <name> <pass>` / `reset_ap`.

### Changing the fallback AP name and password

From a browser: Settings → Access Point (fallback). Or over serial: `set_ap` /
`reset_ap`. Default is `ESP32-Elec-Counter` / `configure123`. This network only
appears when the home link fails — while home WiFi is up the AP stays off.

### Home network (default)

The board joins your home WiFi on boot and the AP stays **OFF**, so the
dashboard lives at `http://<the board's home IP>/` (printed on the serial
console at boot).

From a browser: Settings → Home Network. Or over serial:
`setwifi <ssid> <pass>` / `clearwifi`. `setwifi` saves to flash and reboots;
`clearwifi` forgets it (the fallback AP comes up on next boot).

Defaults live in `src/config.h` as `STA_SSID_DEFAULT` / `STA_PASS_DEFAULT`;
while the SSID is the placeholder the board skips straight to the fallback AP.
If it cannot join within ~10s, the fallback AP comes up instead so the board is
never headless — and if a working link later drops for 30s, the AP comes up
then too.

### Remote monitoring (optional, home WiFi only)

Needs a Firebase Realtime Database (any project — the free tier is plenty)
plus one Email/Password user (Authentication → enable the provider → Add
user). In the Firebase console copy the database host
(`<project>-default-rtdb.<region>.firebasedatabase.app`); the Web API key
and host already ship as firmware defaults.

Rules ship as a file — deploy them with the CLI instead of pasting in the
console (needs `npm i -g firebase-tools`, one time):

```bash
firebase login
firebase use --add        # pick your project (alias stays local, gitignored)
firebase deploy --only database
```

The committed `database.rules.json` lets the board write only its own
`/devices/<MAC>` node while you read with any authenticated client:

```json
{
  "rules": {
    "devices": {
      "$dev": {
        ".write": "true",
        ".read": "auth != null"
      }
    }
  }
}
```

(The board signs in with an ID token, so `.validate` is enforced on its
writes too - unlike a database secret, which would bypass the rules.)

From a browser: Settings → Remote Monitoring (host + account email +
password, PIN-gated). Or over serial:
`setcloud <host> <email> <password>` / `clearcloud`. Saving reboots;
pushes land at `/devices/<MAC>/latest` every second while home WiFi is up.
The Status line shows the MAC and the last-push age; enabling cloud keeps
eco off (a napping radio cannot push). Nothing is ever pushed on the
fallback AP.

### Updating the firmware over the air

Needs home WiFi (the fallback AP has no internet). Each release attaches
ready-to-flash files named `esp32-classic-X.Y.Z.bin` /
`esp32-s3-X.Y.Z.bin` under `.../releases/download/X.Y.Z/`.

From a browser: Settings → Firmware → Check, then Update. The click sends
one console line (`ota <url>`) — serial, dashboard and cloud console share
that single path, so there is no second verb to keep in step. The board
downloads the file straight from github.com into its inactive OTA slot,
verifies it, reboots, and the About card shows the new version (the card
renders the compiled `FIRMWARE_VERSION`, so a release binary is stamped
before compiling — flashing a `.bin` still stamped with the old number
shows the old number forever).

Releases are cut with one command (stamp + both targets + manifest +
GitHub release holding both `.bin` files):

```bash
python3 scripts/build.py --version X.Y.Z
```

Or over serial: `version` (compiled stamp + chip), `update` (check
`version.json`, stage the matching asset, reboot into the updater),
`ota <url>` / `ota status`. The board also checks `version.json` by itself
once an hour and prints one `NEW VERSION!` line per release (silent when
already latest) — `ota status` keeps showing it until you `update`. A tag page or any non-release
link is refused before anything is armed. A failed or oversize download
keeps the old firmware running; counters and settings survive the reboot.

### Status LED

| LED | Meaning |
|---|---|
| Blinking yellow | OTA update in progress |
| Solid red (ON on a plain LED) | No home network — join the fallback AP or use serial |
| Off | Idling normally on home WiFi |

Trips do **not** use the LED (they already have the buzzer + dashboard). The
trip buzzer repeats the channel number in beeps (ch3 = 3 beeps) at 40ms
on/off, with a 1s silence between rounds so two tripped channels never blur
into one long count.

### Clock, event log, eco mode

- **Clock:** on home WiFi the board syncs itself over internet NTP — no phone
  needed. On the fallback AP (no internet) the dashboard lends its clock
  instead. Either way the monthly reset only fires with a valid clock.
- **Event log:** every event (trips, resets, rollovers, boot) is written to
  flash immediately — the last 20 survive any reboot or power cut and show in
  the dashboard's event list.
- **Eco mode:** with nobody watching the dashboard for 60s the WiFi modem naps
  to cut heat; sensing and counting never stop. Opening the dashboard wakes it
  instantly. The Connection panel shows `ECO` vs `Full`, plus the chip
  temperature (classic ESP32 only, approximate — the S3 has no sensor).
- **Admin PIN:** changing it asks twice; a mismatch is refused before anything
  is sent, so one typo can't lock you out.
- **Installing the app:** the Install button lives in Settings → Connection
  (the connect screen's button vanishes once connected). Browsers won't
  auto-install from an `http` board address, so the button shows manual steps:
  Android → menu → Add to Home screen, iPhone → Share → Add to Home Screen.

### Limits

- No access from outside your own WiFi
- No true PWA install on desktop browsers
- The board never sleeps

Serial console (`help` for the list): `status`, `wifi`, `set_ap`, `setwifi`,
`led normal|rgb`, `test led`, `cal`, `inject`, `reboot`, …

---

3.1.0 notes: local-midnight billing (UTC+7), reboot no longer wipes counters, forward-only rollover with unset-marker anchoring, execute-once cloud downlink, guarded `test_force_rollover CONFIRM`.

3.2.0 notes: cloud OTA — `ota <url>` console verb (serial / dashboard / cloud), github release `.bin` into the inactive slot with TLS chain validation, progress banner + chip-aware Firmware panel, `--classic` + `--version` build flags.

3.2.1 notes: fixes a one-character cloud-payload typo (stray quote before `"time"`) that made every push invalid JSON — boards on 3.2.0 poll fine but show straight fails and never update `latest`. Downlink still works, so 3.2.0 boards can take this over the air.

3.2.2 notes: OTA re-test — same code as 3.2.1 with a version stamp only, to prove the end-to-end Check → Update path again.

3.2.3 notes: diagnoses the 3.2.2 `HTTP -1` (board never connected — DNS/route vs TLS was indistinguishable). The download now probes plain TCP first and a failed TLS names heap + clock state in `ota status`. Also fixes the progress banner: the flag cleared on entry so it never showed during a fetch; `cloudActive` now spans the whole download.

3.2.4 notes: version-stamp re-test from the USB-flashed 3.2.3 board — same code, proving Check → Update end to end.

3.2.12 notes: stamp-only target for the 3.2.11 updater — same code. Expecting the same 34 KB wall; its failure line closes the structural case, then OTA goes around board-TLS.

3.2.11 notes: updater wins the race — consumes the staged link on STA (no clock needed), then waits for the clock INSIDE the updater, so no SDK tick can ever start first. Same heap wall expected if the ceiling is structural; the failure line's max number decides.

3.2.10 notes: stamp-only proof for the 3.2.9 updater — same code, taken over the air after one USB flash of 3.2.9.

3.2.9 notes: reboot-to-updater — `ota <url>` only stages the link into NVS and reboots; the first network tick with STA + clock downloads it BEFORE the SDK sessions exist (3.2.8 on 3.2.7: sessions freed but largest block stuck at 34 KB). Last failure kept in NVS so `ota status` survives the reboot; any-case status kept.

3.2.8 notes: version-stamp re-test on the 3.2.7 downloader — same code, proving Check → Update end to end over the air (flash 3.2.7 once over USB, then take this one remotely).

3.2.7 notes: frees the two Firebase keep-alive TLS sessions at download entry (`ota status` on 3.2.6: heap 78 KB but largest block 34 KB, raw-TLS NO — fragmentation, not the chain). `CloudPush::releaseSessions()` + 200 ms settle before the github.com probe; SDK reconnects on next use. `ota status` now answers in any case.

3.2.6 notes: visible-text re-test — Firmware hint gains "Takes about a minute" and the `ota status` idle line reads "since boot", proving Check → Update end to end with observable strings on both sides.

3.2.5 notes: splits the 3.2.4 `TLS failed` (TCP ok, clock ok, 78 KB heap) — reports the largest contiguous block plus a handshake-only raw-TLS probe, so `ota status` tells fragmentation apart from chain validation. The probe never sends HTTP or firmware; the download stays validated.

3.0.0 notes: AP-only architecture, NVS-persisted AP name/password, CLI LED
type switch, pinned-down AsyncTCP patches. Full history: `gh release list`.

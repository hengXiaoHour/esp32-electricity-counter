#!/usr/bin/env bash
# Build the firmware. This is the ONLY supported build entry point.
#
#   ./scripts/build.sh                    # compile to ./.build-out (ESP32-S3)
#   ./scripts/build.sh --classic         # ESP32 classic (min_spiffs, keeps OTA)
#   ./scripts/build.sh --version X.Y.Z   # stamp FIRMWARE_VERSION first
#   ./scripts/build.sh --clean            # full rebuild, no cache
#   ./scripts/build.sh --output-dir DIR   # where to put the .bin
#
# Why this script exists at all
# -----------------------------
# src/network/web_assets.h is GENERATED from frontend/ and is deliberately NOT
# committed. It is 151 KB - a fifth of the working tree - and it appeared five
# separate times in this repo's history, once per migration phase, for about
# 940 KB of duplicated blobs that no one ever read.
#
# The obvious risk of not committing it is "I edited frontend/ and forgot to
# re-embed, so the firmware serves the old page". That is handled structurally
# rather than by a check: this script regenerates the header on EVERY build,
# so a stale copy can never reach the flash. There is no state to forget.
#
# The two things that can still break a build are both checked here, with an
# actionable message instead of a compiler error 200 lines deep:
#   1. the AsyncTCP 1.1.4 patches being absent (see step 2)
#   2. a stale header, which cannot happen - but is re-verified in step 1 anyway
set -euo pipefail

cd "$(dirname "$0")/.." || exit 2

FQBN_S3="esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=no_fs,CDCOnBoot=cdc"
FQBN_CLASSIC="esp32:esp32:esp32:PartitionScheme=min_spiffs"
FQBN="$FQBN_S3"
CLASSIC=0
NEWVER=""
ARDUINO_CLI="${ARDUINO_CLI:-$HOME/.local/bin/arduino-cli}"
OUTDIR="./.build-out"
CLEAN=""

while [ $# -gt 0 ]; do
  case "$1" in
    --classic) CLASSIC=1; FQBN="$FQBN_CLASSIC"; shift ;;
    --version) NEWVER="$2"; shift 2 ;;
    --clean) CLEAN="--clean"; shift ;;
    --output-dir) OUTDIR="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
  esac
done

step() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

# --- 0. Version stamp ----------------------------------------------------
# The About card and the cloud viewer show the COMPILED version, so a release
# binary must carry its own number: flashing a .bin still stamped 3.1.0 shows
# 3.1.0 forever, and the viewer would offer the "update" again. This edits
# exactly one line in src/config.h (asserted below) - the bump IS the commit.
if [ -n "$NEWVER" ]; then
  case "$NEWVER" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "bad version '$NEWVER' (want X.Y.Z)" >&2; exit 2 ;;
  esac
  grep -q '^#define FIRMWARE_VERSION "' src/config.h || {
    echo "FIRMWARE_VERSION line not found in src/config.h" >&2; exit 2; }
  sed -i "s/^#define FIRMWARE_VERSION \".*\"/#define FIRMWARE_VERSION \"$NEWVER\"/" src/config.h
  [ "$(grep -c '^#define FIRMWARE_VERSION "' src/config.h)" -eq 1 ] || {
    echo "version stamp hit != 1 line - refusing" >&2; exit 2; }
  step "Stamped FIRMWARE_VERSION $NEWVER"
  grep '^#define FIRMWARE_VERSION "' src/config.h
fi

# --- 1. Regenerate the embedded dashboard assets ------------------------
# Always, unconditionally. Never "only if frontend/ changed" - that is the
# optimisation that eventually ships the wrong page.
step "Embedding frontend/ into src/network/web_assets.h"
python3 scripts/embed_web.py

step "Verifying the generated header round-trips byte-for-byte"
python3 scripts/embed_web.py --check

# --- 2. The AsyncTCP core-locking compatibility gate ---------------------
# On AsyncTCP 1.1.4 three patches are required and are lost on every
# `arduino-cli lib install`/upgrade: without them the build dies at
# ESPAsyncWebServer.h:1699, or - worse - at runtime with an LWIP core-lock
# assert that still compiles cleanly. AsyncTCP 3.x fixes all of that upstream,
# so the patcher stands down and this gate becomes a no-op.
#
# Check, don't auto-patch: the patcher edits files inside ~/.arduino15 and that
# should be a thing you asked for, not a build side effect.
step "Checking AsyncTCP core-locking compatibility"
if ! python3 scripts/patch_async_tcp.py --check; then
  echo >&2
  echo "AsyncTCP is not in a usable state." >&2
  echo "On 1.1.4 the patches are missing or partial - fix with:" >&2
  echo "    python3 scripts/patch_async_tcp.py" >&2
  echo "On 3.x the version was not recognised; reinstall AsyncTCP 3.x." >&2
  exit 1
fi

# --- 3. Compile ---------------------------------------------------------
step "Compiling ($FQBN)"
mkdir -p "$OUTDIR"
if command -v "$ARDUINO_CLI" >/dev/null 2>&1; then
  CLI="$ARDUINO_CLI"
elif command -v arduino-cli >/dev/null 2>&1; then
  CLI=arduino-cli
else
  echo >&2
  echo "arduino-cli not found. Install it, or set ARDUINO_CLI=/path/to/arduino-cli." >&2
  exit 1
fi

# --output-dir is mandatory: arduino-cli's DEFAULT build dir is ./build inside
# the sketch folder, which silently drops 4 MB of binaries next to the sources
# and leaves a stale .bin that looks exactly like something to flash.
"$CLI" compile --fqbn "$FQBN" --warnings all $CLEAN --output-dir "$OUTDIR"

step "Built"
ls -1 "$OUTDIR"/*.bin 2>/dev/null || true
echo
if [ "$CLASSIC" -eq 1 ]; then
  PORT_HINT="/dev/ttyUSB0"; CHIP="esp32"
else
  PORT_HINT="/dev/ttyACM0"; CHIP="esp32s3"
fi
echo "Flash it with (app only - this keeps the NVS partition, and with it your"
echo "counters, the admin PIN and any access-point name you have set):"
echo "  esptool --port $PORT_HINT --chip $CHIP --baud 460800 \\"
echo "          write-flash 0x10000 $OUTDIR/esp32-electricity-counter.ino.bin"
echo
echo "Only when the bootloader or the partition table changed, flash those too:"
echo "  esptool --port $PORT_HINT --chip $CHIP --baud 460800 write-flash \\"
echo "          0x0 $OUTDIR/esp32-electricity-counter.ino.bootloader.bin \\"
echo "          0x8000 $OUTDIR/esp32-electricity-counter.ino.partitions.bin"
echo
echo "Do NOT flash the .merged.bin for a normal firmware update. It is padded to"
echo "the full 4 MB, so it also overwrites the NVS partition at 0x9000 and wipes"
echo "the energy counters, the admin PIN and the saved access-point credentials."
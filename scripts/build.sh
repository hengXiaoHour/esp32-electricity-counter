#!/usr/bin/env bash
# Build the firmware. This is the ONLY supported build entry point.
#
#   ./scripts/build.sh                    # compile to ./.build-out
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

FQBN="esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=no_fs,CDCOnBoot=cdc"
ARDUINO_CLI="${ARDUINO_CLI:-$HOME/.local/bin/arduino-cli}"
OUTDIR="./.build-out"
CLEAN=""

while [ $# -gt 0 ]; do
  case "$1" in
    --clean) CLEAN="--clean"; shift ;;
    --output-dir) OUTDIR="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
  esac
done

step() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

# --- 1. Regenerate the embedded dashboard assets ------------------------
# Always, unconditionally. Never "only if frontend/ changed" - that is the
# optimisation that eventually ships the wrong page.
step "Embedding frontend/ into src/network/web_assets.h"
python3 scripts/embed_web.py

step "Verifying the generated header round-trips byte-for-byte"
python3 scripts/embed_web.py --check

# --- 2. The AsyncTCP patches must be present ----------------------------
# These are lost on every `arduino-cli lib install`/upgrade. Without them the
# build dies at ESPAsyncWebServer.h:1699, or - worse - at runtime with an
# LWIP core-lock assert. Check, don't auto-patch: the patcher edits files inside
# ~/.arduino15 and that should be a thing you asked for, not a build side effect.
step "Checking AsyncTCP 1.1.4 patches"
if ! python3 scripts/patch_async_tcp.py --check; then
  echo >&2
  echo "AsyncTCP patches are MISSING or PARTIALLY applied." >&2
  echo "Fix with:  python3 scripts/patch_async_tcp.py" >&2
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
echo "Flash it with:"
echo "  esptool --port /dev/ttyACM0 --chip esp32s3 --baud 460800 write-flash 0x0 $OUTDIR/esp32-electricity-counter.ino.bin"
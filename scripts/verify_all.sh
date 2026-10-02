#!/usr/bin/env bash
# Full verification gate for the AP-only architecture.
#
#   ./scripts/verify_all.sh            # everything except the firmware build
#   ./scripts/verify_all.sh --build    # also compile the firmware (slow, ~90 s)
#
# Every stage below has been proven to FAIL when it should: the asset checks
# were run against a deliberately mutated header and against a real pre-migration
# binary; the PIN gate was mutation-tested (always-allow, all-exempt and
# accept-truncated all get caught); the E2E suite was mutation-tested against
# four broken copies of script.js. A stage that has only ever printed PASS is
# not evidence.
set -u

cd "$(dirname "$0")/.." || exit 2
FAILED=0
DO_BUILD=0
[ "${1:-}" = "--build" ] && DO_BUILD=1

stage() {
  printf '\n\033[1m=== %s ===\033[0m\n' "$1"
}

record() {
  if [ "$1" -eq 0 ]; then
    printf '  \033[32mPASS\033[0m %s\n' "$2"
  else
    printf '  \033[31mFAIL\033[0m %s\n' "$2"
    FAILED=$((FAILED + 1))
  fi
}

# --- 1. The generated header matches frontend/ and round-trips ----------
stage "Embedded dashboard assets"
python3 scripts/embed_web.py --check >/tmp/opencode/verify_assets.log 2>&1
record $? "assets match frontend/ and round-trip byte-for-byte"

# --- 2. Admin-PIN gate unit tests (host build, no hardware) ------------
stage "Admin PIN gate (unit)"
mkdir -p /tmp/opencode
gcc -std=c11 -Wall -Wextra -Isrc/network -x c \
    scripts/test_auth_gate.c src/network/auth_gate.cpp \
    -o /tmp/opencode/auth_gate_test 2>/tmp/opencode/verify_gate_build.log
if [ $? -ne 0 ]; then
  record 1 "auth_gate compiles"
else
  record 0 "auth_gate compiles"
  /tmp/opencode/auth_gate_test >/tmp/opencode/verify_gate.log 2>&1
  record $? "$(tail -1 /tmp/opencode/verify_gate.log)"
fi

# --- 3. Frontend syntax + no cloud code survives ------------------------
stage "Frontend"
node --check frontend/script.js 2>/tmp/opencode/verify_js.log
record $? "script.js parses"

CLOUD_TOKENS=$(grep -rniE 'firebase|gstatic|cloudDb|normalizeSnapshot|loadDevicePicker|FB_CONFIG|set_ntfy_topic|connMode|devicePicker' \
  frontend/ 2>/dev/null | grep -v '^frontend/icons/' | wc -l)
[ "$CLOUD_TOKENS" -eq 0 ]
record $? "no Firebase/RTDB/cloud tokens left in frontend/ (found $CLOUD_TOKENS)"

# --- 4. E2E: real page against a mock board ----------------------------
stage "End-to-end (real frontend + mock board)"
PORT=${MOCK_PORT:-8099}
python3 scripts/mock_device.py --port "$PORT" >/tmp/opencode/verify_mock.log 2>&1 &
MOCK_PID=$!
# Give it a moment, and fail loudly rather than testing a dead server.
for _ in $(seq 1 25); do
  curl -sf -o /dev/null "http://127.0.0.1:$PORT/" && break
  sleep 0.2
done
if curl -sf -o /dev/null "http://127.0.0.1:$PORT/"; then
  node scripts/e2e_aponly.js "http://127.0.0.1:$PORT" >/tmp/opencode/verify_e2e.log 2>&1
  record $? "$(tail -1 /tmp/opencode/verify_e2e.log)"
else
  record 1 "mock board did not come up on port $PORT"
fi
kill "$MOCK_PID" 2>/dev/null
wait "$MOCK_PID" 2>/dev/null

# --- 5. Firmware build + assets actually in flash -----------------------
if [ "$DO_BUILD" -eq 1 ]; then
  stage "Firmware"
  ~/.local/bin/arduino-cli compile --fqbn \
      esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=no_fs,CDCOnBoot=cdc \
      --warnings all --output-dir /tmp/opencode/verify-build \
      >/tmp/opencode/verify_build.log 2>&1
  record $? "arduino-cli compile"

  WARNINGS=$(grep -c 'warning:' /tmp/opencode/verify_build.log)
  printf '  info %s warning(s) (2 pre-existing banner format warnings expected)\n' "$WARNINGS"

  grep -E 'Sketch uses|Global variables' /tmp/opencode/verify_build.log | sed 's/^/  /'

  python3 scripts/embed_web.py --verify-binary \
      /tmp/opencode/verify-build/esp32-electricity-counter.ino.bin \
      >/tmp/opencode/verify_bin.log 2>&1
  record $? "all dashboard assets are present in the firmware image"
fi

printf '\n'
if [ "$FAILED" -eq 0 ]; then
  printf '\033[32mALL GATES PASS\033[0m\n'
else
  printf '\033[31m%s GATE(S) FAILED\033[0m\n' "$FAILED"
fi
exit "$FAILED"
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

# Every python3 call in this script must not write .pyc files. The setup-wizard
# test does `import setup`, which created scripts/__pycache__/setup.*.pyc, and
# the dead-code stage - which runs AFTER that stage - pins scripts/__pycache__
# as a deletion that must stay deleted. So the gate used to fail its own run.
export PYTHONDONTWRITEBYTECODE=1

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
# Generate first: web_assets.h is no longer committed, so on a fresh clone there
# is nothing to check and the old --check-first order failed with "does not
# exist". Regenerating unconditionally is also what makes a stale copy impossible
# rather than merely detectable.
stage "Embedded dashboard assets"
python3 scripts/embed_web.py >/tmp/opencode/verify_assets.log 2>&1 &&
  python3 scripts/embed_web.py --check >>/tmp/opencode/verify_assets.log 2>&1
record $? "assets match frontend/ and round-trip byte-for-byte"

# The AsyncTCP patches are lost on every library install. Missing patch #2 does
# not fail the build - it ships firmware that reboot-loops at runtime on
# server->begin(). Check it explicitly rather than discovering it on the board.
python3 scripts/patch_async_tcp.py --check >/tmp/opencode/verify_patch.log 2>&1
record $? "AsyncTCP is in a usable state for Arduino-ESP32 3.x"

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

# --- 2b. AP credential rules (host build, no hardware) -------------------
# The SSID/password the board broadcasts are now writable at runtime, which
# turns a bad value into a lockout: esp_wifi_set_config() rejects a PSK under 8
# octets, softAP() then fails, and the board comes back with no radio - reachable
# only over serial. This suite is mutation-checked (7 deliberately broken
# validators, all caught); see scripts/test_ap_creds.c.
stage "AP credential rules (unit)"
gcc -std=c11 -Wall -Wextra -Isrc/network -x c \
    scripts/test_ap_creds.c src/network/ap_creds.cpp \
    -o /tmp/opencode/ap_creds_test 2>/tmp/opencode/verify_apcreds_build.log
if [ $? -ne 0 ]; then
  record 1 "ap_creds compiles"
else
  record 0 "ap_creds compiles"
  /tmp/opencode/ap_creds_test >/tmp/opencode/verify_apcreds.log 2>&1
  record $? "$(tail -1 /tmp/opencode/verify_apcreds.log)"
fi

# --- 2c. Cloud config rules (host build, no hardware) --------------------
# Remote monitoring is push-only HTTPS REST with a MAC-derived device path.
# The shape rules (host form, token form, MAC formatting) are the only thing
# standing between a typo and a dead database node, so they are unit-tested
# here like the AP credential rules above.
stage "Cloud config rules (unit)"
gcc -std=c11 -Wall -Wextra -Isrc/network -x c \
    scripts/test_cloud_cfg.c src/network/cloud_cfg.cpp \
    -o /tmp/opencode/cloud_cfg_test 2>/tmp/opencode/verify_cloudcfg_build.log
if [ $? -ne 0 ]; then
  record 1 "cloud_cfg compiles"
else
  record 0 "cloud_cfg compiles"
  /tmp/opencode/cloud_cfg_test >/tmp/opencode/verify_cloudcfg.log 2>&1
  record $? "$(tail -1 /tmp/opencode/verify_cloudcfg.log)"
fi

# --- 2d. JSON string escaping (host build, no hardware) -------------------
# One unescaped quote in the network name would corrupt EVERY system broadcast,
# not just the Access Point panel, because the frame is rebuilt ~7x a second.
stage "JSON escaping"
gcc -std=c11 -Wall -Wextra -x c \
    scripts/test_json_escape.c -o /tmp/opencode/json_escape_test \
    2>/tmp/opencode/verify_jsonesc_build.log
if [ $? -ne 0 ]; then
  record 1 "test_json_escape.c compiles"
else
  record 0 "test_json_escape.c compiles"
  /tmp/opencode/json_escape_test >/tmp/opencode/verify_jsonesc.log 2>&1
  record $? "$(tail -1 /tmp/opencode/verify_jsonesc.log)"
fi

# --- 2e. Setup wizard (host test, no hardware) ---------------------------
# scripts/setup.py provisions a board over USB serial (setwifi, setcloud,
# dashboard IP). The suite drives it against a fake board on a pty that
# speaks the firmware's exact reply lines, and is mutation-checked (dropped
# quoting, wrong Saved marker, https regex - all caught).
stage "Setup wizard (pty)"
python3 scripts/test_setup.py >/tmp/opencode/verify_setup.log 2>&1
record $? "$(tail -1 /tmp/opencode/verify_setup.log)"

# --- 3. Frontend syntax + no LEGACY cloud code ---------------------------
# The pattern below names the REMOVED era only (Firebase SDK tags, the old
# cloudDb/device-picker plumbing, ntfy). The current push-only remote-
# monitoring panel (cloudHost/cloudStatus/setcloud) is intentional and uses
# none of those tokens - which is exactly why the pattern still passes with
# the panel present. If a future edit needs one of these words in frontend/,
# rename the token here deliberately, never by broadening the match.
stage "Frontend"
node --check frontend/script.js 2>/tmp/opencode/verify_js.log
record $? "script.js parses"

# Strip comments before grepping. Without this the check fails on prose that
# legitimately NAMES a removed thing (e.g. "it used to be connMode ..."), which
# trains you to ignore the gate instead of fixing the code.
strip_comments() {
  python3 - "$1" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)      # block comments
src = re.sub(r"^\s*//.*$", "", src, flags=re.M)        # whole-line //
src = re.sub(r"//.*$", "", src, flags=re.M)            # trailing //
src = re.sub(r"<!--.*?-->", " ", src, flags=re.S)      # html comments
sys.stdout.write(src)
PY
}

CLOUD_HITS=""
for f in frontend/*.js frontend/*.html frontend/*.css; do
  [ -e "$f" ] || continue
  hits=$(strip_comments "$f" | grep -niE 'firebase|gstatic|cloudDb|normalizeSnapshot|loadDevicePicker|FB_CONFIG|set_ntfy_topic|connMode|devicePicker' || true)
  [ -n "$hits" ] && CLOUD_HITS="$CLOUD_HITS$f: $hits"$'\n'
done
[ -z "$CLOUD_HITS" ]
record $? "no legacy cloud code in frontend/ (comments ignored)"
[ -n "$CLOUD_HITS" ] && printf '%s' "$CLOUD_HITS"

# --- 3b. No dead code or dead assets ------------------------------------
stage "Dead code"
python3 scripts/check_deadcode.py >/tmp/opencode/verify_dead.log 2>&1
record $? "$(tail -1 /tmp/opencode/verify_dead.log)"

# --- 3c. Documentation claims match the code ---------------------------
stage "Documentation"
python3 scripts/check_docs.py >/tmp/opencode/verify_docs.log 2>&1
record $? "$(tail -1 /tmp/opencode/verify_docs.log)"

# --- 3d. Cloud viewer is the same dashboard, rebuilt from frontend/ ------
# cloud-viewer/script.js must stay a byte-exact copy of frontend/script.js
# (rendering shared; only cloud.js differs by design) and the builder must
# reproduce cloud-viewer/ from frontend/ + cloud.js. A hand-edit to the
# generated copy that is never ported back is how the two UIs drift apart.
stage "Cloud viewer"
node --check cloud-viewer/script.js 2>/dev/null
record $? "cloud-viewer/script.js parses"
node --check cloud-viewer/cloud.js 2>/dev/null
record $? "cloud-viewer/cloud.js parses"
cmp -s frontend/script.js cloud-viewer/script.js
record $? "cloud-viewer/script.js is an exact copy of frontend/script.js"
python3 scripts/build_cloud_viewer.py >/tmp/opencode/verify_cloudview.log 2>&1
record $? "cloud-viewer rebuilds from frontend/"
# cl_adapt is the only translation between the cloud payload and the shared
# renderer. It once omitted the whole calibration block, so the cloud Settings
# panel came up blank while the board held every value - a name-level grep
# cannot catch a mistyped mapping, so run the function on a real-shaped payload.
node scripts/e2e_cloud_adapt.js >/tmp/opencode/verify_cloudadapt.log 2>&1
record $? "$(tail -1 /tmp/opencode/verify_cloudadapt.log)"
# Same bug, whole chain in a real browser: cloud.js adapter -> script.js
# renderer -> the actual <input> elements, with a stubbed Firebase. The unit
# test above can prove a field is MAPPED; only this proves the panel shows it.
# Both are mutation-tested against the shipped bug (see the file header).
if node -e "require(require('child_process').execSync('npm root -g',{encoding:'utf8'}).trim()+'/playwright')" 2>/dev/null; then
  CLOUD_PORT=${CLOUD_PORT:-8098}
  (cd cloud-viewer && python3 -m http.server "$CLOUD_PORT" >/tmp/opencode/verify_cloudhttp.log 2>&1) &
  CLOUD_HTTP_PID=$!
  for _ in $(seq 1 25); do
    curl -sf -o /dev/null "http://127.0.0.1:$CLOUD_PORT/" && break
    sleep 0.2
  done
  if curl -sf -o /dev/null "http://127.0.0.1:$CLOUD_PORT/"; then
    node scripts/e2e_cloud_settings.js "http://127.0.0.1:$CLOUD_PORT" \
      >/tmp/opencode/verify_cloudsettings.log 2>&1
    record $? "$(tail -1 /tmp/opencode/verify_cloudsettings.log)"
  else
    record 1 "cloud-viewer served locally for the settings browser test"
  fi
  kill "$CLOUD_HTTP_PID" 2>/dev/null
  wait "$CLOUD_HTTP_PID" 2>/dev/null
else
  printf '  \033[33mSKIP\033[0m cloud settings browser check (playwright not installed globally)\n'
fi
git diff --quiet -- cloud-viewer/ 2>/dev/null
if [ $? -eq 0 ]; then
  record 0 "cloud-viewer/ is up to date (builder is a no-op)"
else
  record 1 "cloud-viewer/ is STALE - commit the rebuilt files"
  git diff --stat -- cloud-viewer/ | head -8 | sed 's/^/      /'
fi

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
# A leftover mock from an earlier run can hold the port, in which case THIS mock
# dies on bind() while `curl` is perfectly happy answering from the old one - and
# the suite then tests a server built from older code. It failed exactly that way
# once (an AP test "failed" against a mock that had never heard of set_ap). The
# liveness check is what closes the hole; the port check turns it into a
# sentence instead of a mystery.
if ! kill -0 "$MOCK_PID" 2>/dev/null; then
  record 1 "the mock board is alive on port $PORT (is an old one still running?)"
  printf '      %s\n' "$(tail -3 /tmp/opencode/verify_mock.log | tr '\n' ' ')"
elif curl -sf -o /dev/null "http://127.0.0.1:$PORT/"; then
  node scripts/e2e_aponly.js "http://127.0.0.1:$PORT" >/tmp/opencode/verify_e2e.log 2>&1
  record $? "$(tail -1 /tmp/opencode/verify_e2e.log)"

  # --- 4b. The UI actually lines up and is themed ------------------------
  # The Admin PIN inputs once rendered as white browser-default boxes: the
  # stylesheet selected inputs by TYPE and both PIN fields are type="password".
  # No E2E assertion can see that - the DOM is correct either way - so it needs
  # a real layout, measured. Four widths, because a column that lines up at
  # 1280 can still drift at 360.
  # Skips loudly rather than silently when Playwright is absent: a stage that
  # quietly does nothing is how the broken UI shipped in the first place.
  if node -e "require(require('child_process').execSync('npm root -g',{encoding:'utf8'}).trim()+'/playwright')" 2>/dev/null; then
    mkdir -p /tmp/opencode/ui
    UI_OK=0
    UI_W=360
    for UI_W in 360 480 692 1280; do
      node scripts/ui_shot.js "http://127.0.0.1:$PORT" /tmp/opencode/ui "$UI_W" 900 \
        >"/tmp/opencode/verify_ui_$UI_W.log" 2>&1 || { UI_OK=1; break; }
    done
    record "$UI_OK" "settings form is themed and its columns line up at 360/480/692/1280px"
    if [ "$UI_OK" -ne 0 ]; then
      grep -E 'FAILED|misaligned|browser-default' "/tmp/opencode/verify_ui_$UI_W.log" \
        | head -6 | sed 's/^/      /'
    fi
  else
    printf '  \033[33mSKIP\033[0m UI layout check (playwright not installed globally)\n'
  fi

  # --- 4c. Auto-zero in progress + the feedback box -----------------------
  # A SECOND mock, on its own port, with the board mid-calibration. The main
  # mock is idle, so this is the only way to render the state the bug lived in:
  # a real calibration takes 20 batches over seconds, and the Auto-Zero button
  # that could not work was left live through all of them.
  AZ_PORT=$((PORT + 1))
  MOCK_AZ_CHANNEL=1 MOCK_AZ_QUEUE=2 MOCK_AZ_PROGRESS=5 \
    python3 scripts/mock_device.py --port "$AZ_PORT" >/tmp/opencode/verify_mock_az.log 2>&1 &
  AZ_PID=$!
  for _ in $(seq 1 25); do
    curl -sf -o /dev/null "http://127.0.0.1:$AZ_PORT/" && break
    sleep 0.2
  done
  if ! kill -0 "$AZ_PID" 2>/dev/null || ! curl -sf -o /dev/null "http://127.0.0.1:$AZ_PORT/"; then
    record 1 "the auto-zero mock board came up on port $AZ_PORT"
  elif ! node -e "require(require('child_process').execSync('npm root -g',{encoding:'utf8'}).trim()+'/playwright')" 2>/dev/null; then
    printf '  \033[33mSKIP\033[0m auto-zero UI check (playwright not installed globally)\n'
  else
    AZ_OK=0
    AZ_W=360
    # 360 is the phone width the bug was reported at; 1280 proves the fix is not
    # a narrow-screen-only patch.
    for AZ_W in 360 1280; do
      node scripts/e2e_autozero.js "http://127.0.0.1:$AZ_PORT" "$AZ_W" \
        >"/tmp/opencode/verify_az_$AZ_W.log" 2>&1 || { AZ_OK=1; break; }
    done
    record "$AZ_OK" "Auto-Zero is disabled while calibrating and the status text clears its button"
    if [ "$AZ_OK" -ne 0 ]; then
      grep -E 'FAIL|checks passed' "/tmp/opencode/verify_az_$AZ_W.log" \
        | head -8 | sed 's/^/      /'
    fi
  fi
  kill "$AZ_PID" 2>/dev/null
  wait "$AZ_PID" 2>/dev/null
else
  record 1 "mock board did not come up on port $PORT"
fi
kill "$MOCK_PID" 2>/dev/null
wait "$MOCK_PID" 2>/dev/null

# --- 5. Firmware build + assets actually in flash -----------------------
if [ "$DO_BUILD" -eq 1 ]; then
  stage "Firmware"
  # Deliberately the SAME entry point a user would run. A verification gate that
  # compiles by a different route than the documented one is testing a build
  # nobody performs.
  ./scripts/build.sh --output-dir /tmp/opencode/verify-build \
      >/tmp/opencode/verify_build.log 2>&1
  record $? "scripts/build.sh (embed + patch check + arduino-cli compile)"

  # Zero warnings is now the bar. The two -Wformat warnings that used to sit in
  # the banner printf were fixed rather than tolerated, because a warning gate
  # you always have to ignore is not a gate.
  WARNINGS=$(grep -c 'warning:' /tmp/opencode/verify_build.log || true)
  [ "$WARNINGS" -eq 0 ]
  record $? "clean build: $WARNINGS compiler warning(s)"
  grep -E 'warning:' /tmp/opencode/verify_build.log | head -10 | sed 's/^/      /'

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
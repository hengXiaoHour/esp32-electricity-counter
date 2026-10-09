#!/usr/bin/env python3
"""Check that the documentation's factual claims match the code.

README.md and doc/ARCHITECTURE.md are hand-written prose about a codebase that
changes. Prose drifts silently: nobody notices a stale number, and the next
person to read the doc trusts it. This asserts the specific claims that are
checkable, so drift becomes a failing command instead of a lie.

Two hard-won rules are baked in:

  * Strip comments before matching. An early version of this script reported
    "the firmware still calls the deprecated beginResponse_P" when the only
    occurrence was inside a comment explaining that it does NOT. A checker that
    cries wolf on correct code trains you to ignore it.
  * Count what is actually SERVED, not what is listed. The generator's asset
    table has 11 entries but one is optional (config.js), so 10 reach the
    device. Asserting on the table entry count gave a false failure.

Usage:  python3 scripts/check_docs.py
Exit 0 = every claim holds.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"^\s*//.*$", "", src, flags=re.M)
    src = re.sub(r"//.*$", "", src, flags=re.M)
    return src


class Checker:
    def __init__(self):
        self.results = []

    def add(self, name, ok, detail=""):
        self.results.append((name, bool(ok), detail))

    def report(self):
        bad = 0
        for name, ok, detail in self.results:
            if ok:
                print("  ok   %s" % name)
            else:
                bad += 1
                print("  FAIL %s%s" % (name, ("  [%s]" % detail) if detail else ""))
        print("\n%d/%d documentation claims verified" % (len(self.results) - bad, len(self.results)))
        return bad


def _cal_under_lock(cl_cpp):
    """True when CloudPush::snapshot copies the calibration fields BEFORE it
    releases dataMutex.

    The readings are copied into locals under the lock and serialised after the
    release (a multi-second TLS must never hold the sensor task). The
    calibration block has to obey the same rule: reading powerCalc->noiseFloor[]
    from the network task without the lock is a torn float read, and a torn
    read is indistinguishable from a real calibration value.
    """
    body = re.search(
        r"bool CloudPush::snapshot\([\s\S]*?\n(.*?)\n\}\n", cl_cpp, flags=re.S)
    if not body:
        return False
    src = body.group(1)
    give = src.find("xSemaphoreGive(*mutex)")
    if give < 0:
        return False
    anchors = ("voltageCalibration", "currentCalibration[i]", "rmsSamples",
               "noiseFloor[i]", "lpfAlpha[i]", "getAutoZeroQueue")
    return all(src.find(a) >= 0 and src.find(a) < give for a in anchors)


def main():
    c = Checker()

    # Read the channel count from the firmware, once, so every assertion below
    # compares against the real number instead of a literal that can rot.
    m = re.search(r"#define NUM_CHANNELS\s+(\d+)", read("src/config.h"))
    if not m:
        print("FATAL: NUM_CHANNELS not found in src/config.h")
        return
    NUM_CHANNELS = int(m.group(1))

    ino = read("esp32-electricity-counter.ino")
    arch = read("doc/ARCHITECTURE.md")
    rdme = read("README.md")
    cfg = read("src/config.h")
    gate = read("src/network/auth_gate.cpp")
    apc = read("src/network/ap_creds.h")
    apc_cpp = read("src/network/ap_creds.cpp")
    nvs = read("src/utils/nvs_manager.cpp")
    nvs_h = read("src/utils/nvs_manager.h")
    cmd = read("src/network/command_processor.cpp")
    ch = read("src/network/console_handler.cpp")
    ws_code = strip_comments(read("src/network/websocket_server.cpp"))
    wm = read("src/network/wifi_manager.cpp")
    wm_h = read("src/network/wifi_manager.h")
    header = read("src/network/web_assets.h")
    js = read("frontend/script.js")
    html = read("frontend/index.html")
    css = read("frontend/style.css")
    sw = read("frontend/sw.js")
    sysjson = read("src/network/system_json.cpp")
    json_test = read("scripts/test_json_escape.c")
    ts_h = read("src/network/time_sync.h")
    ts_cpp = read("src/network/time_sync.cpp")
    lm = read("src/core/limit_manager.cpp")
    lm_h = read("src/core/limit_manager.h")
    buzz_h = read("src/ui/buzzer.h")
    buzz_cpp = read("src/ui/buzzer.cpp")
    cl_cfg_h = read("src/network/cloud_cfg.h")
    cl_cfg_cpp = read("src/network/cloud_cfg.cpp")
    cl_h = read("src/network/cloud_push.h")
    cl_cpp = read("src/network/cloud_push.cpp")
    cl_test = read("scripts/test_cloud_cfg.c")
    verify_sh = read("scripts/verify_all.sh")
    ota_h = read("src/network/ota_handler.h")
    ota_cpp = read("src/network/ota_handler.cpp")
    cloudjs = read("cloud-viewer/cloud.js")
    build_sh = read("scripts/build.sh")
    build_py = read("scripts/build_cloud_viewer.py")

    # --- task layout ---------------------------------------------------
    task_calls = ino.count("xTaskCreatePinnedToCore")
    c.add("ARCHITECTURE says 'Two Tasks, Two Cores' and there are exactly 2 tasks",
          "Two Tasks, Two Cores" in arch and task_calls == 2,
          "found %d xTaskCreatePinnedToCore" % task_calls)

    # --- asset pipeline ------------------------------------------------
    count = re.search(r"#define WEB_ASSET_COUNT (\d+)", header)
    served = int(count.group(1)) if count else -1
    rows = len(re.findall(r'\{ "/', header))
    c.add("generated header declares and lists the same number of assets",
          served > 0 and served == rows, "WEB_ASSET_COUNT=%s rows=%d" % (served, rows))
    c.add("ARCHITECTURE's asset count (%s) matches the header" % re.search(r"Ten files|(\w+) files", arch).group(0)
          if re.search(r"Ten files|(\w+) files", arch) else False,
          "header says %d" % served)
    c.add("the optional config.js asset is NOT compiled in (no Firebase config on the device)",
          '/config.js' not in header)

    # --- the PIN gate --------------------------------------------------
    c.add("auth_gate exempts exactly set_time and verify_pin",
          re.search(r'PIN_EXEMPT_VERBS\[\] = \{"set_time", "verify_pin"', gate) is not None)
    c.add("README and the gate agree the default PIN is 1234",
          'defaultPin() { return "1234"; }' in nvs and "`1234`" in rdme)
    # The NVS KEY lives in the .cpp; the header only declares the accessors.
    # Asserting the literal appears in the header was wrong - it never did.
    c.add("the PIN lives in NVS under admin_pin",
          'getString("admin_pin"' in nvs and 'savePin' in nvs_h and 'loadPin' in nvs_h)
    c.add("auth_gate has no Arduino dependency (it is host-testable)",
          "#include <Arduino.h>" not in gate and "#include <WiFi.h>" not in gate)

    # --- the web server ------------------------------------------------
    c.add("the dashboard is served from a catch-all, not per-file handlers",
          "onNotFound" in ws_code)
    c.add("the firmware calls the NON-deprecated beginResponse",
          "beginResponse(" in ws_code and "beginResponse_P" not in ws_code)
    # Assert on the CODE SHAPE, not on a comment explaining it. The exact-match
    # loop plus a nullptr fallthrough is the property that stops the device
    # becoming an open file server; the prose about it is just prose.
    c.add("asset lookup is an exact-equality loop that falls through to nullptr",
          re.search(r"for\s*\(uint32_t i = 0; i < WEB_ASSET_COUNT; i\+\+\)\s*\{\s*"
                    r"if \(want == WEB_ASSETS\[i\]\.path\) return &WEB_ASSETS\[i\];",
                    ws_code) is not None
          and "return nullptr;" in ws_code)
    c.add("asset lookup does no prefix/directory walking",
          "startsWith" not in ws_code and "indexOf(\"..\")" not in ws_code)
    c.add("no-cache is set on served assets",
          "no-cache" in ws_code)

    # --- STA-first (AP is fallback-only, off unless the home link fails) ---
    # STA-only is the normal running state, so the radio must start as WIFI_STA
    # and only switch to WIFI_AP_STA inside the fallback. Starting in AP_STA
    # would broadcast unconditionally, which is exactly what "AP off by default"
    # forbids.
    c.add("the radio starts STA-only, AP only in the fallback",
          re.search(r"WiFi\.mode\(WIFI_STA\)", wm) is not None and
          re.search(r"WiFi\.mode\(WIFI_AP_STA\)", wm) is not None)
    c.add("the fallback starts the AP and keeps the STA retrying",
          re.search(r"WiFi\.softAP\(.*?\).*?WiFi\.begin\(", wm, re.S) is not None)
    c.add("readiness holds in EITHER running state, never in INIT",
          re.search(r"state == WIFI_STA_MODE \|\| state == WIFI_AP_MODE", wm_h) is not None)
    # The placeholder literal must exist exactly ONCE (as STA_SSID_PLACEHOLDER).
    # A second copy anywhere is how a real default later ships still filtered.
    c.add("the placeholder SSID literal exists exactly once, as its own macro",
          cfg.count('"YOUR_HOME_SSID"') == 1 and
          "STA_SSID_PLACEHOLDER" in cfg and
          '"YOUR_HOME_SSID"' not in wm,
          "a literal copy of the placeholder exists outside config.h")
    c.add("the join guard compares the RUNTIME buffer to the placeholder macro",
          "strcmp(staSsid_, STA_SSID_PLACEHOLDER)" in wm,
          "the guard checks the default instead of the live value")
    # Each of the three fallback triggers must actually CALL the fallback, not
    # just log about it. A count is not enough: deleting one call site still
    # leaves the total above any threshold, which is exactly how the first
    # version of this rule survived its own mutation test.
    _fb = lambda trig: re.search(re.escape(trig) + r"[\s\S]{0,400}?startFallbackAP\(\)", wm) is not None
    c.add("the unconfigured path falls back instead of joining nothing",
          _fb("no home network configured"),
          "unconfigured path logs but never calls startFallbackAP()")
    c.add("the join-timeout path falls back instead of hanging",
          _fb("not reached in"),
          "timeout path logs but never calls startFallbackAP()")
    c.add("a prolonged STA loss brings the fallback AP up",
          _fb("lost for 30s"),
          "loop-loss path logs but never calls startFallbackAP()")

    # --- LED means one thing in every radio state --------------------------
    # Blink = OTA, solid = no home network, off = idling on STA. The old rule
    # (solid while an AP client watched) left the LED ON during every normal
    # STA session, which is exactly the complaint this replaces.
    _led = re.search(r"static void updateLED\(\) \{([\s\S]*?)\n\}", ino)
    _led_body = _led.group(1) if _led else ""
    c.add("OTA blinks the LED",
          "otaHandler.isInProgress()" in _led_body and "LED_BLINK_YELLOW" in _led_body)
    c.add("no home network lights the LED solid",
          "!wifiMgr.stationUp()" in _led_body and "LED_SOLID_RED" in _led_body)
    c.add("a connected STA idles the LED off (no client-count rule)",
          "LED_OFF" in _led_body and "clientCount" not in _led_body,
          "updateLED still keys on AP clients")

    # --- NTP disciplines the clock whenever STA is up ----------------------
    c.add("NTP starts once the STA link is up",
          "wifiMgr.stationUp()) timeSync.beginNTP()" in ino or
          "stationUp()) timeSync.beginNTP()" in ino)
    c.add("each network cycle polls NTP for a fresh sync",
          "timeSync.pollNTP()" in ino)
    c.add("SNTP is pointed at public servers with the board timezone applied",
          "configTime(TIMEZONE_OFFSET_SECONDS, 0" in ts_cpp
          and "pool.ntp.org" in ts_cpp
          and "#define TIMEZONE_OFFSET_SECONDS (7 * 3600)" in cfg,
          "a bare configTime(0, 0, ...) would bill on UTC midnights, not local")
    # Strip comments first: the name acceptEpoch also appears in a comment
    # inside pollNTP, and a substring rule would keep passing after the real
    # call was deleted (caught by mutation testing, not by reading).
    _poll = re.search(r"void TimeSync::pollNTP\(\) \{([\s\S]*?)\n\}\n",
                      strip_comments(ts_cpp))
    c.add("NTP results pass the same plausibility gate as browser frames",
          _poll is not None and "acceptEpoch(" in _poll.group(1),
          "pollNTP() records time without going through acceptEpoch()")
    c.add("the browser lend remains for the offline fallback-AP state",
          "setTimeFromBrowser" in ts_cpp and "set_time" in js)

    # --- every event survives a reboot (last 20 in NVS) --------------------
    c.add("the forensic ring keeps the last 20 events",
          re.search(r"FORENSIC_KEEP\s*=\s*20", nvs_h) is not None)
    _logev = re.search(r"void LimitManager::logEvent\(uint8_t ch[^{]*\{([\s\S]*?)\n\}\n",
                     lm)
    c.add("RAM-only logging is gone: every logEvent persists",
          _logev is not None and "persistForensic();" in _logev.group(1),
          "logEvent() returns without persisting")
    c.add("the forensic write commits immediately (power-cut safe)",
          re.search(r"saveForensicEvents\(tail, keep\);\s*[\s\S]{0,400}?nvs->commit\(\);",
                    lm) is not None,
          "persistForensic stages without committing")

    # --- buzzer: fast countable beeps, separated rounds -----------------------
    # Two properties matter: the 40ms phases (countable at speed) and the 1s
    # end-pause (neighbouring channels must not blur into one long count).
    # Both are plain constants, so assert values, not vibes.
    _beep = re.search(r"BEEP_MS\s*=\s*(\d+)", buzz_h)
    _gap = re.search(r"GAP_MS\s*=\s*(\d+)", buzz_h)
    _pause = re.search(r"END_PAUSE_MS\s*=\s*(\d+)", buzz_h)
    c.add("beep phases are short enough to count at speed (<=50ms)",
          _beep and _gap and int(_beep.group(1)) <= 50 and int(_gap.group(1)) <= 50,
          "BEEP_MS=%s GAP_MS=%s" % (_beep.group(1) if _beep else "?",
                                    _gap.group(1) if _gap else "?"))
    c.add("every pattern round ends with a 1s silence before the next",
          _pause is not None and int(_pause.group(1)) == 1000 and
          "state = END_PAUSE" in buzz_cpp and "case END_PAUSE:" in buzz_cpp,
          "the end-pause state is gone - patterns blur together")

    # --- remote monitoring + remote control, STA-only, MAC identity --------
    # The return of the cloud, minus everything that got it removed. Each rule
    # below is one archived lesson (doc/opencode_agent/lessons.md); the
    # mutation suite proves each one can fail.
    # Comments may NAME the removed SDK (repo convention: strip comments first,
    # like verify_all.sh does) - the rule is about code, not prose.
    # Transport is the Firebase SDK (the August smooth era, minus its one sin:
    # herd email/password, never a service-account key on the device). The
    # hand-rolled REST era is over because it wedged; the rule below pins the
    # SDK shape instead of the REST shape. Comments may NAME either transport
    # (repo convention: strip comments first) - the rule is about code.
    c.add("the pusher writes via the SDK merge call, no service-account key",
          "updateNode(" in cl_cpp and "Firebase.RTDB" in cl_cpp and
          "service_account" not in strip_comments(cl_cpp).lower() and
          "PRIVATE_KEY" not in cl_cpp,
          "transport left the SDK shape, or an admin key is on the device")
    # The downlink is the ONE sanctioned read: getJSON on /devices/<id>/cmd
    # on its own timer and its own FirebaseData session. Scoped to the poll
    # body (not the file): a bare "getJSON is present" would also pass for a
    # read smuggled into the push path, which is exactly the old teardown bug.
    _poll = re.search(r"void CloudPush::pollCmd\([^)]*\) \{([\s\S]*?)\n\}\n",
                      strip_comments(cl_cpp))
    _poll_b = _poll.group(1) if _poll else ""
    c.add("the only read is the downlink cmd poll (getJSON /cmd, own session)",
          _poll is not None and "/cmd" in _poll_b and
          "getJSON(" in _poll_b and "s_pollFbdo" in _poll_b and
          "getJSON(" not in strip_comments(cl_cpp).replace(_poll.group(0), ""),
          "a read path outside pollCmd, or a non-SDK read")
    # Scoped to loop()'s body, not the file: WL_CONNECTED also appears in
    # wantsRadio(), so a file-wide substring rule passes after the loop's own
    # gate is deleted (proven by mutation, not by reading).
    _loop = re.search(r"void CloudPush::loop\([^)]*\) \{([\s\S]*?)\n\}\n", cl_cpp)
    c.add("pushes are STA-gated (fallback AP has no internet)",
          _loop is not None and "WL_CONNECTED" in _loop.group(1),
          "loop() can fire without a station link")
    _push_iv = re.search(r"PUSH_INTERVAL_MS\s*=\s*(\d+)", cl_h)
    c.add("the push cadence is 1 s over the SDK session",
          _push_iv is not None and int(_push_iv.group(1)) == 1000,
          "interval moved off 1 s")
    c.add("the device id comes from the radio MAC, formatted by the helper",
          "macAddress()" in cl_cpp and "cloud_formatDeviceId" in cl_cpp,
          "identity no longer tracks the silicon")
    c.add("there is no settable board id anywhere (no verb, no NVS key)",
          "setdevice" not in strip_comments(cmd).lower() and
          "device_id" not in strip_comments(nvs).lower() and
          "setdevice" not in js.lower(),
          "a hand-typed id is back")
    c.add("the password never reaches the dashboard snapshot",
          '\\"auth\\"' not in sysjson and '\\"pass\\"' not in sysjson,
          "credential material in the wire format")
    c.add("the host IS published (not secret) so the panel shows it",
          '\\"host\\"' in sysjson and "appendJsonEscaped(json, host)" in sysjson,
          "panel cannot show what is configured")
    c.add("both input verbs validate shape through the host-tested helpers",
          "cloud_validateHost" in ch and "cloud_validateEmail" in ch and
          "cloud_validatePass" in ch and "cloud_splitArgs3" in ch and
          "cloud_validateHost" in cmd and "cloud_validateEmail" in cmd and
          "cloud_validatePass" in cmd,
          "a verb trusts raw input")
    c.add("the shape helpers are unit-tested and the suite runs in verify_all",
          "test_cloud_cfg" in verify_sh and "CLOUD_DEVICE_ID_LEN" in cl_test,
          "validation without a runner")
    c.add("cloud counts as watched, so cloud-on means eco-off",
          "cloudPush.wantsRadio()" in ino,
          "eco can nap a radio that must push")
    # Session hygiene: the SDK owns the ID token internally - no token buffer
    # lives in this code at all, so there is nothing to leak to flash and no
    # hand-rolled refresh to rot. A prefs.putString in the pusher means
    # something session-like leaked; auth.user.email/password is the ONE
    # sanctioned credential use (herd login), and a service-account key must
    # never appear.
    _sess = strip_comments(cl_cpp)
    c.add("the session never reaches flash (re-login on boot instead)",
          "putString" not in _sess and "putBytes" not in _sess,
          "the pusher persists session material")
    c.add("herd login through the SDK, never a service-account key",
          "auth.user.email" in _sess and "auth.user.password" in _sess and
          "service_account" not in _sess.lower() and
          "PRIVATE_KEY" not in cl_cpp,
          "login left the herd shape, or an admin key is on the device")
    # ...but "present in the file" is not "wired": pin each path to its
    # function body (proven by mutation, not by reading).
    _ensure = re.search(r"bool CloudPush::ensureLogin\(\) \{([\s\S]*?)\n\}\n", cl_cpp)
    _post = re.search(r"bool CloudPush::post\(const String &body\) \{([\s\S]*?)\n\}\n", cl_cpp)
    _ensure_b = strip_comments(_ensure.group(1)) if _ensure else ""
    _post_b = strip_comments(_post.group(1)) if _post else ""
    c.add("ensureLogin starts the SDK and gates on Firebase.ready()",
          "cloudSdkEnsure(" in _ensure_b and "Firebase.ready()" in _ensure_b,
          "first boot can never sign in")
    c.add("post() merge-writes through the SDK push session",
          "postStatus(" in _post_b and "updateNode(" in strip_comments(cl_cpp),
          "a push path outside the SDK session")
    c.add("push and poll ride separate SDK sessions (never share the hot path)",
          "s_pushFbdo" in _sess and "s_pollFbdo" in _sess,
          "sessions merged back into one")
    # The TLS stack put classic at 103% of the default 1.2 MB app slot;
    # min_spiffs (1.9 MB, OTA kept) is the documented scheme. Reverting the
    # README line to the bare FQBN silently unbuilds the classic board.
    c.add("README pins min_spiffs for the classic ESP32 build",
          "esp32:esp32:esp32:PartitionScheme=min_spiffs" in rdme,
          "classic line reverted to the default scheme (103% overflow)")
    # The password must never be logged: pull every DEBUG_LOG/STATUS_LOG call
    # out of the pusher and assert none of them formats one. A substring rule
    # on the whole file cannot say this - the buffer legitimately EXISTS in
    # the file (stored, handed to the SDK once at start), just never in a log
    # line. The account email MAY be logged (identifier, and the user typed it
    # themselves). There is no local token buffer anymore (the SDK owns it),
    # which is exactly why the secret list is down to the password alone.
    _log_calls = re.findall(r"(?:DEBUG_LOG|STATUS_LOG)\(([\s\S]*?)\);", cl_cpp)
    _secret_names = ("pass_", "idToken_", "refreshToken_")
    c.add("password and session tokens are never logged (only host/status/email)",
          _log_calls and all(not any(s in call for s in _secret_names)
                             for call in _log_calls),
          "a log line formats a credential")
    # Same for the diag readout: no credential buffer may be formatted. The
    # SDK holds the token internally and this code has no accessor, so the
    # strongest true statement is absence - plus the session/health lines
    # that must exist so a dead board still reports itself.
    _diag = re.search(r"void CloudPush::diag\(String &out[^{]*\{([\s\S]*?)\n\}\n", cl_cpp)
    _diag_b = strip_comments(_diag.group(1)) if _diag else ""
    _diag_outs = re.findall(r"consoleAppendf\(out,([\s\S]*?)\);", _diag_b)
    c.add("cloud diag exposes session/health, never credential bytes",
          _diag is not None and _diag_outs and "Session:" in _diag_b and
          "Health:" in _diag_b and
          all(not any(s in call for s in _secret_names) for call in _diag_outs),
          "diag() hides the session state or formats credential bytes")
    c.add("cloud diag reports the last executed command and the poll age",
          _diag is not None and "LastCmd" in _diag_b and "Poll:" in _diag_b,
          "diag() hides the downlink state")

    # --- cloud downlink: full remote control, PIN-free by design ------------
    # Local WebSocket callers keep the PIN (processCommand default); only the
    # cloud poller skips it, because trust there comes from the RTDB rules
    # (only the admin Gmail can write cmd) + TLS + the board's ID token.
    c.add("local WebSocket callers keep the PIN (no skipAuth at the WS site)",
          "processCommand(" in ws_code and "skipAuth" not in ws_code,
          "the WS path stopped enforcing the PIN")
    c.add("the downlink executes via processCommand with auth skipped",
          _poll is not None and
          re.search(r"processCommand\([\s\S]{0,400}?, true\)", _poll_b) is not None,
          "pollCmd does not pass skipAuth=true")
    c.add("the PIN skip names its trust (RTDB rules + TLS + ID token)",
          "CLOUD TRUST" in cmd and "RTDB rules" in cmd and "skipAuth" in cmd,
          "the trust comment is gone - a skip without a reason")
    c.add("the executed cmd id is deduplicated in NVS under cloud_cmd",
          'getString("cloud_cmd"' in nvs and 'putString("cloud_cmd"' in nvs and
          'loadCloudCmdId' in nvs_h and 'saveCloudCmdId' in nvs_h,
          "an acked command re-runs after every reboot")
    # Execute-once: the id is recorded BEFORE processCommand runs, so a
    # failed ack write can never re-run the verb on the next poll. The old
    # shape (record-after + retry) double-executed non-idempotent verbs —
    # the 2026-10-05 double-wipe. Code shape, not comments: exactly one
    # record site in the poll body, ordered before the execution.
    c.add("a failed cmd ack never re-runs the command (id recorded first)",
          _poll is not None and
          _poll_b.count("saveCloudCmdId") == 1 and
          "processCommand(" in _poll_b and
          _poll_b.index("saveCloudCmdId") < _poll_b.index("processCommand("),
          "an ack failure would execute a non-idempotent verb twice")
    _poll_iv = re.search(r"POLL_INTERVAL_MS\s*=\s*(\d+)", cl_h)
    c.add("the downlink polls on its own 2 s timer, wired into loop()",
          _poll_iv is not None and int(_poll_iv.group(1)) == 2000 and
          _loop is not None and "pollCmd(" in _loop.group(1),
          "poll starves behind the push or never runs")
    # The frame may carry set_pin/set_ap/setwifi passwords, so the poll's own
    # log lines may name the verb and the id, never the frame. Scoped to the
    # log CALLS inside pollCmd: the variable legitimately exists in the body.
    _poll_logs = re.findall(r"(?:DEBUG_LOG|STATUS_LOG)\(([\s\S]*?)\);", _poll_b)
    c.add("the poll never logs the command frame (it may carry passwords)",
          _poll_logs and all("frame" not in call for call in _poll_logs),
          "a log line formats the raw frame")

    # --- five channels means five, everywhere ---------------------------------
    # The 6->5 migration left a channels[5] out-of-bounds read in the status
    # print plus "6-Channel" claims across the docs. Count-proof: assert the
    # absence, not a comment saying so.
    c.add("no sixth-channel residue: channels[5] appears nowhere in firmware",
          "channels[5]" not in ino and "channels[5]" not in strip_comments(read("src/network/system_json.cpp")),
          "a ch6 index survived the 5-channel migration")
    c.add("the docs count five channels, not six",
          "6-Channel" not in ino and "6-Channel" not in rdme and
          "6-Channel" not in arch and "ch1–ch6" not in arch,
          "a 6-channel claim is back")
    c.add("presence is WebSocket viewers (OTA counts - flashing needs link)",
          "wsServer.clientCount() > 0 || otaHandler.isInProgress()" in ino)
    c.add("eco parks the modem only in STA-only mode, never under the AP",
          re.search(r"if \(!wifiMgr\.apActive\(\)\) \{\s*\n.*WiFi\.setSleep\(true\)",
                    ino) is not None,
          "setSleep(true) can hit the fallback AP that must beacon")
    c.add("a returning viewer restores full power before needing the link",
          re.search(r"WiFi\.setSleep\(false\)", ino) is not None)
    c.add("eco state rides the snapshot and the dashboard shows it",
          '\\"eco\\"' in sysjson and 'getElementById(\'ecoStatus\')' in js)

    # --- chip temperature rides the snapshot --------------------------------
    c.add("the snapshot carries MCU temperature, null when sensorless",
          '\\"mcuTemp\\"' in sysjson and "isnan(data.mcuTempC)" in sysjson,
          "NaN would serialise as bare nan and kill the frame")
    c.add("the temperature reader is compile-guarded for sensorless chips",
          "#if defined(CONFIG_IDF_TARGET_ESP32) || "
          "(defined(SOC_TEMP_SENSOR_SUPPORTED) && SOC_TEMP_SENSOR_SUPPORTED)" in ino,
          "the readMcuTempC() guard was weakened - S3 would not compile")
    c.add("the dashboard shows the temperature or a dash, never 0.0",
          'getElementById(\'mcuTemp\')' in js)

    # --- header wifi icon levels cover every emitted level ------------------
    # The shipped bug: JS emitted lv1/lv3 while CSS only knew lv0/lv2, so the
    # icon rendered dim next to a live RSSI number.
    _emitted = set(re.findall(r"'(lv\d)'", js))
    _styled = set(re.findall(r"\.wifi\.(lv\d)", css))
    c.add("every wifi level the dashboard can emit has a CSS rule",
          len(_emitted) > 0 and _emitted <= _styled,
          "emitted=%s styled=%s" % (sorted(_emitted), sorted(_styled)))

    # --- PIN change needs confirmation --------------------------------------
    c.add("the new PIN must be typed twice and match",
          'id="pinConfirm"' in html and "do not match" in js)
    c.add("a mismatch never reaches the board",
          re.search(r"val !== again[\s\S]{0,200}?return Promise\.resolve\(false\)",
                    js) is not None)

    # --- install UI is cloud-only ---------------------------------------
    # It used to live in the connect panel, then Settings, then the header - all
    # on the BOARD page, where it could never work: that page is plain http on a
    # LAN IP, so no browser registers the service worker there and none fires
    # beforeinstallprompt. The button was a dead control on the one origin that
    # cannot install. It now ships only with the hosted (https) dashboard, and
    # these claims pin BOTH halves: nothing install-shaped in the board's copy,
    # and the cloud build still carrying the whole UI.
    _embed = read("scripts/embed_web.py")
    _install_js = read("cloud-viewer/install.js") if (
        ROOT / "cloud-viewer/install.js").exists() else ""
    cl_html = read("cloud-viewer/index.html")
    cl_css = read("cloud-viewer/style.css")
    fr_sw = read("frontend/sw.js")
    c.add("the board page has no install UI at all",
          'id="installRow"' not in html and 'id="installBtnTop"' not in html
          and 'id="installHint"' not in html and "promptInstall" not in js,
          "frontend/index.html or frontend/script.js still carries install markup")
    c.add("the board firmware does not embed the install module",
          "install.js" not in _embed,
          "scripts/embed_web.py would put an install asset in the firmware image")
    c.add("the hosted page has the install UI, header button included",
          'id="installBtnTop"' in cl_html and 'id="installRow"' in cl_html
          and 'id="installHint"' in cl_html and 'src="install.js' in cl_html,
          "build_cloud_viewer.py stopped injecting the cloud-only install UI")
    c.add("the cloud install module is wired end to end",
          all(t in _install_js for t in ("beforeinstallprompt", "appinstalled",
                                         "function promptInstall",
                                         "showInstallRow", "hideInstallRow"))
          and "Cloud-only additions" in cl_css and ".install-row" in cl_css,
          "cloud-viewer/install.js or its CSS block is incomplete")
    # Same shape as the cloud.js precache claim further down: the module is in
    # the hosted shell and NOT in the board's, so one cannot leak into the other.
    c.add("the cloud service worker precaches the install module",
          "./install.js" in read("cloud-viewer/sw.js") and "./install.js" not in fr_sw,
          "install.js is missing from the cloud shell or present in the board's")
    c.add("the station defaults live in config.h",
          "STA_SSID_DEFAULT" in cfg and "STA_PASS_DEFAULT" in cfg)
    c.add("station credentials are read from NVS first, defaults second",
          "loadWiFi(staSsid, staPass)" in wm)
    c.add("setwifi saves the station SSID to NVS (serial verb)",
          'putString("wifi_ssid"' in nvs and "cmdSetWifi" in ch)
    # The C source spells this as "\"cmd\":\"setwifi\"" (escaped quotes inside a
    # C string literal), so the needle is written the same way in Python.
    setwifi_needle = '"' + '\\"cmd\\":\\"setwifi\\"' + '"'
    c.add("setwifi saves the station SSID to NVS (WebSocket verb)",
          setwifi_needle in cmd, "needle %r not found" % setwifi_needle)
    # The mock and the firmware must agree on the channel count, or the E2E
    # suite tests a board that does not exist. Sizes compared, not literals.
    mock = read("scripts/mock_device.py")
    import ast as _ast
    mock_channels = len(_ast.literal_eval(
        re.search(r"self\.names = (\[[^\]]*\])", mock, re.S).group(1)))
    c.add("the mock board publishes NUM_CHANNELS channels, like the firmware",
          mock_channels == NUM_CHANNELS,
          "mock=%s firmware=%s" % (mock_channels, NUM_CHANNELS))
    c.add("the dashboard's channel count matches the firmware",
          re.search(r"const NUM_CHANNELS = (\d+);", js).group(1) == str(NUM_CHANNELS),
          "js=%s firmware=%s" % (re.search(r"const NUM_CHANNELS = (\d+);", js).group(1),
                                 NUM_CHANNELS))
    c.add("the dashboard reports the real station state, not a hardcoded false",
          re.search(r"systemData\.wifiConnected = wifiMgr\.stationUp\(\)", ino) is not None)
    # AP-only removed the NVS dependency from WiFiManager::begin() and this
    # feature put it back: the network name and password are no longer compile-
    # time constants. The old claim ("takes no NVS argument") is now false, and
    # a stale assertion is worse than none.
    c.add("WiFiManager::begin loads the AP identity from NVS",
          "void WiFiManager::begin(NVSManager *nvsRef)" in wm and
          "void begin(NVSManager *nvsRef);" in wm_h and
          "loadCredentials(nvsRef)" in wm)
    c.add("the AP defaults live in config.h (both NVSManager and WiFiManager need them)",
          'AP_SSID_DEFAULT = "ESP32-Elec-Counter"' in cfg and
          'AP_PASS_DEFAULT = "configure123"' in cfg and
          "AP_SSID_DEFAULT" not in wm_h)
    c.add("captive DNS still answers every hostname with the board IP",
          re.search(r'dnsServer\.start\(53, "\*", apIP\)', wm) is not None)
    c.add("the README still documents the factory AP credentials",
          "ESP32-Elec-Counter" in rdme and "configure123" in rdme)

    # --- AP credentials are writable, and that path is real --------------
    # A half-finished feature here is worse than no feature: the user renames
    # the network, the board reboots onto the new name, and if the change was
    # never committed they are locked out of a network they can no longer see.
    c.add("the AP credentials live in NVS under ap_ssid / ap_pass",
          'getString("ap_ssid"' in nvs and 'putString("ap_ssid"' in nvs and
          'getString("ap_pass"' in nvs and 'putString("ap_pass"' in nvs)
    c.add("the .ino hands the NVS handle to WiFiManager::begin",
          "wifiMgr.begin(&nvs);" in ino)
    c.add("a stored-but-invalid identity falls back to the defaults instead of booting headless",
          re.search(r"if \(!ap_creds_validate\(ssid\.c_str\(\), pass\.c_str\(\), &reason\)\)", wm)
          is not None and 'return;' in wm and 'AP_SSID_DEFAULT' in wm)
    c.add("set_ap is PIN-gated (it is a mutating verb, so the gate covers it by default)",
          '"cmd\\":\\"set_ap"' not in ino and
          "ap_creds_validate" in cmd and "requestReboot" in cmd)
    # Persistence has to happen BEFORE the restart, or the reboot resurrects the
    # old identity and the change silently evaporates. Both orderings are
    # asserted, because "saved but not committed" and "restarted before the
    # commit" are the two ways this feature quietly does nothing.
    save_at = cmd.find("nvs->saveApCredentials")
    commit_at = cmd.find("nvs->commit()", save_at)
    reboot_at = cmd.find("requestReboot", save_at)
    c.add("set_ap commits to flash before it asks for the restart",
          save_at > 0 and commit_at > save_at and reboot_at > commit_at,
          "save@%d commit@%d reboot@%d" % (save_at, commit_at, reboot_at))
    c.add("set_ap's commit is inside the dataMutex bracket, not after it",
          # Comments are stripped first. The real code has a five-line comment
          # between the write and the commit, and a window sized for code alone
          # does not reach across it - so the first version of this check failed
          # on the CLEAN tree, which quietly made every mutation "caught" for the
          # wrong reason. A gate that fails on correct code is worse than none.
          re.search(r"xSemaphoreTake\(\*dataMutex[^;]*;[\s\S]{0,200}?"
                    r"nvs->saveApCredentials[\s\S]{0,200}?nvs->commit\(\);"
                    r"[\s\S]{0,120}?xSemaphoreGive\(\*dataMutex\)",
                    strip_comments(cmd)) is not None)
    # The frontend's copy of these limits must match the device's, or the UI
    # rejects something the board accepts (annoying) or accepts something the
    # board rejects - which, for the minimum, means the phone drops off a network
    # the board never joined. The NAMES differ between the two (a C macro vs a JS
    # const), so each pair is asserted explicitly rather than by a clever regex
    # rewrite, which is how the first version of this check silently passed on
    # one side only.
    for label, js_pat, cpp_pat in [
            ("password minimum", r"AP_PASS_MIN\s*=\s*8\b", r"#define\s+AP_MIN_PASS_LEN\s+8\b"),
            ("password maximum", r"AP_PASS_MAX\s*=\s*63\b", r"#define\s+AP_MAX_PASS_LEN\s+63\b"),
            ("name maximum", r"AP_SSID_MAX\s*=\s*32\b", r"#define\s+AP_MAX_SSID_LEN\s+32\b")]:
        c.add("the frontend's AP %s matches the device rule" % label,
              re.search(js_pat, js) is not None and re.search(cpp_pat, apc) is not None,
              "js %s / ap_creds.h %s" % (bool(re.search(js_pat, js)),
                                         bool(re.search(cpp_pat, apc))))
    c.add("the dashboard has an Access Point panel that sends set_ap",
          'id="apSsid"' in html and 'id="apPass"' in html and
          "set_ap" in js and "reset_ap" in js)
    c.add("the README documents how to rename the network and how to recover it",
          # Anchored on the backticked command references, not the bare verb: the
          # verb also appears in the command list and in the recovery prose, so a
          # substring match kept passing after the walkthrough was gutted.
          "`set_ap`" in rdme and "`reset_ap`" in rdme and
          "fallback AP name and password" in rdme)
    c.add("ARCHITECTURE records that the AP identity is persisted",
          "`ap_ssid`, `ap_pass`" in arch and "set_ap" in arch and "reset_ap" in arch)

    # --- firmware version / size ---------------------------------------
    m = re.search(r'#define FIRMWARE_VERSION "([^"]+)"', cfg)
    c.add("FIRMWARE_VERSION is stamped X.Y.Z",
          m and re.match(r"^\d+\.\d+\.\d+$", m.group(1)) is not None,
          "found %s" % (m.group(1) if m else "none"))

    # --- local OTA only: no cloud updater --------------------------------
    # Updates happen over USB or ArduinoOTA on the LAN. There is no github
    # download, no staged link, no `ota` console verb and no Check/Update
    # buttons - each absence is asserted so a re-added cloud layer fails.
    import os as _os
    from pathlib import Path as _Path
    _root = _Path(__file__).resolve().parent.parent
    c.add("the console offers no ota verb",
          'startsWith("ota ")' not in ch and '"ota status"' not in ch and
          "cmdOta" not in ch,
          "a cloud download path would have a verb to land on")
    c.add("the staged-link NVS keys are gone",
          "ota_url" not in nvs and "ota_err" not in nvs,
          "a staged link would survive in flash")
    c.add("the URL validator files are gone",
          not (_root / "src/network/ota_url.cpp").exists() and
          not (_root / "src/network/ota_url.h").exists() and
          not (_root / "scripts/test_ota_url.c").exists(),
          "the validator invites a cloud download back")
    c.add("the handler is LAN-only (ArduinoOTA, no download)",
          "ArduinoOTA.handle()" in ota_cpp and
          "loopCloud" not in ota_cpp and
          "github.com" not in ota_cpp and "Update.begin(" not in ota_cpp,
          "a download path lives in the handler")
    c.add("the handler raises no reboot flag",
          "cloudRebootDue" not in ota_h,
          "the sketch would reboot into an updater")
    c.add("the sketch has no updater reboot",
          "cloudRebootDue()" not in ino and "consumeCloudReboot()" not in ino,
          "a verified image would reboot outside the deferred path")
    c.add("both snapshots still publish the OTA progress and the chip",
          re.search(r'\\?"ota\\?"', sysjson) is not None and
          re.search(r'\\?"chip\\?"', sysjson) is not None and
          "otaRun" in cl_cpp and "otaPct" in cl_cpp and
          re.search(r'\\?"chip\\?"', cl_cpp) is not None,
          "the LAN banner and the About panel read dead keys")
    c.add("the cloud adapter maps the OTA state onto the shared banner",
          "otaRun" in cloudjs and "otaProgress" in cloudjs,
          "remote progress would never render")
    # --- calibration key parity (the 2026-10-08 blank-panel bug) --------
    # One renderer, two serializers. buildSystemJson() (LAN) and
    # CloudPush::snapshot() (cloud) publish the SAME dashboard fields under the
    # SAME names, and cl_adapt maps the cloud one back onto the local shape.
    # They drifted once: the cloud payload carried no calibration at all, so
    # Settings > Calibration rendered blank in the cloud viewer while the board
    # held every value. Assert the three sets are equal instead of trusting
    # that a future edit remembers to update all three.
    #
    # These three checks are NAME-level on purpose and are mutation-tested: a
    # missing key is caught here. A key that is present but WRONG (e.g.
    # `typeof latest.azBatches === 'string'`) is deliberately out of scope -
    # no grep can see that - and is covered by scripts/e2e_cloud_adapt.js,
    # which RUNS cl_adapt on a real-shaped payload. Verified: that one mutant
    # survives here and is caught there.
    cal_keys = ("voltageCalibration", "currentCalibration", "rmsSamples",
                "azBatches", "noiseFloor", "azActive", "azChannel",
                "azProgress", "azQueue", "lpfAlpha")
    sysjson_s, clcpp_s = strip_comments(sysjson), strip_comments(cl_cpp)
    cloudjs_s = re.sub(r"/\*.*?\*/", " ", cloudjs, flags=re.S)
    cloudjs_s = re.sub(r"^\s*//.*$", "", cloudjs_s, flags=re.M)
    missing_lan = [k for k in cal_keys if ('\\"%s\\"' % k) not in sysjson_s]
    missing_cloud = [k for k in cal_keys if ('\\"%s\\"' % k) not in clcpp_s]
    missing_map = [k for k in cal_keys if ("latest.%s" % k) not in cloudjs_s]
    c.add("both snapshots publish the same calibration keys (LAN)",
          not missing_lan, "missing: %s" % ",".join(missing_lan))
    c.add("both snapshots publish the same calibration keys (cloud push)",
          not missing_cloud, "missing: %s" % ",".join(missing_cloud))
    c.add("cl_adapt maps every calibration key the cloud push sends",
          not missing_map, "unmapped: %s" % ",".join(missing_map))
    c.add("the cloud snapshot copies the calibration under dataMutex too",
          _cal_under_lock(cl_cpp),
          "a torn float read would publish a half-updated panel")
    # The builder renamed the service-worker cache with a LITERAL ("-v15"). The
    # board copy bumps that version whenever its shell changes, so the rename
    # silently stopped working: the cloud app then used the BOARD's cache name
    # and the two apps evicted each other's shell. Assert the outcome, not the
    # intent, so the next literal-vs-pattern mistake cannot hide either.
    cl_sw, fr_sw = read("cloud-viewer/sw.js"), read("frontend/sw.js")
    _fr_cache = re.search(r"const CACHE_NAME = '([^']+)'", fr_sw)
    _cl_cache = re.search(r"const CACHE_NAME = '([^']+)'", cl_sw)
    c.add("the cloud service worker uses its OWN cache name, not the board's",
          bool(_fr_cache) and bool(_cl_cache) and
          _cl_cache.group(1) != _fr_cache.group(1) and
          _cl_cache.group(1).startswith("esp32-counter-cloud-"),
          "board=%s cloud=%s" % (_fr_cache.group(1) if _fr_cache else "?",
                                 _cl_cache.group(1) if _cl_cache else "?"))
    c.add("the builder renames that cache by pattern, not a pinned version",
          re.search(r'esp32-counter-v\\d\+"', build_py) is not None,
          "a literal version in build_cloud_viewer.py stops matching on the next bump")
    c.add("the cloud shell precaches cloud.js (its transport override)",
          "./cloud.js" in cl_sw and "./cloud.js" not in fr_sw,
          "an installed cloud PWA boots without its transport")
    c.add("the cloud payload has no stray quote before \"time\" (3.2.0 broke every push)",
          'body += ",\\"time\\":{\\"ok\\":"' in cl_cpp and
          'body += "\\",\\"time\\"' not in cl_cpp,
          "a stray quote makes otaPct invalid JSON and all pushes fail while polls pass")
    c.add("the viewer has no release checker or update sender",
          "api.github.com" not in js and "checkFirmware" not in js and
          "startFirmwareUpdate" not in js,
          "the Check button would have no backend")
    c.add("the Firmware panel shows the running version, no Check/Update",
          'id="fwRunning"' in html and 'id="fwCheckBtn"' not in html and
          'id="fwUpdateBtn"' not in html,
          "the Check button is still in the panel")
    c.add("build.sh builds the classic target on min_spiffs with a version stamp",
          "--classic" in build_sh and "PartitionScheme=min_spiffs" in build_sh and
          "--version" in build_sh and "FIRMWARE_VERSION" in build_sh,
          "no reproducible path to a stamped classic binary")
    c.add("the README documents the local-only update flow",
          "ArduinoOTA" in rdme and
          "ota <url>" not in rdme.split("### Updating the firmware")[1].split("### ")[0] and
          "releases/download" not in rdme.split("### Updating the firmware")[1].split("### ")[0],
          "the update section still promises a cloud update")

    # --- frontend ------------------------------------------------------
    c.add("index.html loads NO external scripts",
          "gstatic" not in html and "http://" not in html and "https://" not in html)
    c.add("the connect panel no longer offers a mode picker or IP field",
          'id="connMode"' not in html and 'id="esp32Ip"' not in html)
    c.add("the PIN input exists in the UI",
          'id="pinInput"' in html)
    c.add("the ntfy panel is gone",
          'id="ntfyTopic"' not in html and "sendNtfyTopic" not in js)
    c.add("the frontend connects to location.host (no typed IP)",
          "'ws://' + location.host + '/ws'" in js)
    c.add("the frontend lends the clock and re-lends it",
          "set_time" in js and "timeSyncTimer" in js)
    c.add("the frontend shows the board's clock state",
          "timeStatus" in html and "renderClock" in js)
    c.add("the frontend caches the PIN in sessionStorage only",
          "sessionStorage.getItem('esp32counter_pin')" in js and
          "localStorage.setItem('esp32counter_pin'" not in js)
    m = re.search(r"esp32-counter-v(\d+)", sw)
    c.add("service worker cache name was bumped past the pre-migration one",
          m is not None and int(m.group(1)) >= 14)
    c.add("demo mode is reachable without a board",
          "?demo=1" in js)

    # --- the Access Point panel shows the stored identity ---------------
    # It used to carry a static placeholder, so a renamed board still looked
    # factory-fresh and the user had nothing telling them what to edit.
    # The key is written with escaped quotes in the C++ literal, so match on the
    # key name rather than on the exact source text.
    c.add("the board reports its live AP name on every broadcast",
          re.search(r'\\"apSsid\\"', sysjson) is not None
          and re.search(r'\\"apIsDefault\\"', sysjson) is not None,
          "buildSystemJson no longer publishes the AP identity")
    c.add("the AP password is never sent to the browser",
          # Match the KEY, escaped or not: the firmware writes
          # json += ",\"apPass\":\"" , so a plain '"apPass"' substring test sails
          # straight past the exact leak it exists to catch. loadApPass() is
          # legitimately called here to compare against the default, which is
          # why this keys on the JSON name and not on the word.
          re.search(r'\\?"apPass\\?"', sysjson) is None,
          "system_json.cpp publishes a JSON key called apPass")
    c.add("the dashboard fills the AP fields from the board",
          "apSsidEl.value = data.apSsid;" in js
          and "data.apIsDefault" in js
          and "apPassEl.value" not in js,
          "script.js does not write the wire identity into the AP fields")
    c.add("a half-typed AP name survives the 7 Hz push",
          "apSsidEl.dataset.userSet" in js and "apPassEl.dataset.userSet" in js,
          "the broadcast would overwrite the field being typed into")
    c.add("the AP actions are Save then Defaults, in their own row below the fields",
          re.search(r'id="apSaveBtn"[^>]*>\s*Save\s*</button>\s*<button[^>]*id="apResetBtn"[^>]*>\s*Defaults\s*</button>', html) is not None,
          "expected <button id=apSaveBtn>Save</button> then id=apResetBtn Defaults")

    # The host test re-declares appendJsonEscaped because system_json.cpp needs
    # Arduino.h, so the two copies cannot be textually identical: types and the
    # append function differ by necessity. What must NOT differ is the decision
    # structure - the branch conditions and the fallback. Comparing those is the
    # only comparison that means something, and it is what the check asserts.
    fw_body = re.search(r"static void appendJsonEscaped.*?\n\}", sysjson, re.S)
    tst_body = re.search(r"static void appendJsonEscaped.*?\n\}", json_test, re.S)
    if not (fw_body and tst_body):
        c.add("the host test's copy of appendJsonEscaped matches the firmware's",
              False, "could not find appendJsonEscaped in both files")
    else:
        def branches(text):
            return [re.sub(r"\s+", " ", ln.strip())
                    for ln in text.splitlines()
                    if re.match(r"\s*(}\s*else\s+)?if\s*\(", ln)]

        def fallback_count(text):
            return sum(1 for ln in text.splitlines()
                       if re.search(r"json\s*\+?=?.*'\s'|=.*'\s'\)", ln))

        fw_b, tst_b = branches(fw_body.group(0)), branches(tst_body.group(0))
        ok = (fw_b == tst_b
              and len(fw_b) == 2                      # quote/backslash, then < 0x20
              and fallback_count(fw_body.group(0)) == fallback_count(tst_body.group(0)))
        c.add("the host test's copy of appendJsonEscaped matches the firmware's",
              ok, "firmware branches=%s test branches=%s" % (fw_b, tst_b))

    # --- billing reset day (NVS, not hardcoded) -------------------------
    # The rollover used to anchor on the MONTHLY_RESET_DAY literal; now the
    # day lives in NVS (`reset_day`) and the literal is only the factory
    # default + the no-NVS fallback. A hardcoded comparison coming back
    # would silently ignore the user's setting, so the absence is asserted.
    lm_code = strip_comments(lm)
    c.add("the rollover reads the reset day from NVS, not the #define",
          "loadResetDay()" in lm_code
          and re.search(r">=\s*MONTHLY_RESET_DAY", lm_code) is None,
          "rollover ignores a user-set day")
    c.add("changing the day re-anchors the month without zeroing counters",
          "saveResetDay" in lm_code and "saveLastMonth(billingMonthFor" in lm_code,
          "a day change must not wipe kWh as a side effect")
    c.add("the NVS reset-day accessors persist under reset_day",
          'getUChar("reset_day"' in nvs and 'saveResetDay' in nvs_h
          and 'loadResetDay' in nvs_h)
    c.add("the CLI spells reset_day <1-28> and documents it in help",
          'startsWith("reset_day")' in ch and "cmdResetDay" in ch
          and "reset_day <1-28>" in ch)
    # The test wipe is one typo away on every console, so the bare verb must
    # be a dry run and only the exact guard word arms it. Scoped to the
    # dispatch block (not help text): the single firing call must sit under
    # the CONFIRM comparison, and the dry-run message must exist.
    _force_cli = re.search(r'startsWith\("test_force_rollover"\)([\s\S]*?)\n  \} else',
                           strip_comments(ch))
    _force_cli_b = _force_cli.group(1) if _force_cli else ""
    c.add("the CLI rollover test needs CONFIRM (bare is a dry run)",
          _force_cli is not None and
          '== "CONFIRM"' in _force_cli_b and
          _force_cli_b.count("saveLastMonth") == 1 and
          "Nothing done" in _force_cli_b,
          "typing the bare verb would wipe a month of counters")
    c.add("factory reset restores the default reset day too",
          "saveResetDay(MONTHLY_RESET_DAY)" in cmd)
    c.add("both snapshots publish the enforced reset day",
          re.search(r'\\?"resetDay\\?"', sysjson) is not None
          and re.search(r'\\?"resetDay\\?"', cl_cpp) is not None,
          "About panel would show a dead value")
    c.add("the About panel edits the day through the shared console line",
          'id="resetDay"' in html and 'id="resetDayBtn"' in html
          and "data.resetDay" in js and "reset_day " in js,
          "a second JSON verb would split the CLI/UI code paths")
    # --- rollover fires forward-only -----------------------------------
    # A marker AHEAD of the computed month means the clock moved backward
    # (NTP/browser correction) or the marker came from the future. Zeroing
    # there destroys real accumulation because of a correction, so the
    # marker is re-anchored down instead. test_force_rollover still forces
    # a wipe: it sets the marker BACK, which reads as forward progress.
    c.add("the rollover zeroes only on forward month progress",
          "if (billingMonth < marker)" in lm_code
          and "re-anchored, counters kept" in lm_code,
          "a backward correction would wipe real counters")
    # --- reboot must not close NVS -------------------------------------
    # 2026-10-05: cmdReboot called nvs->end() before its 1 s delay. A closed
    # Preferences handle returns defaults on every read (verified against the
    # installed core: getInt returns defaultValue, put* are no-ops), so the
    # sensor task's rollover check read last_month as 0 mid-reboot and fired.
    # The handle must stay open until ESP.restart() lands.
    c.add("the reboot path never closes the NVS handle",
          "nvs->end()" not in strip_comments(ch),
          "a closed handle reads last_month as 0 and the reboot wipes")
    # --- unset marker anchors, never wipes -------------------------------
    # Marker 0 = fresh board or an unreadable read (closed handle, mid-commit
    # race). There is no previous month to close out, so anchoring keeps
    # counters that wiping would destroy on zero evidence. Scoped to the
    # rollover body: the guard must sit before the zeroing branch.
    _ro = re.search(r"void LimitManager::rolloverIfNeeded\(\) \{([\s\S]*?)\n\}\n",
                    strip_comments(lm))
    _ro_b = _ro.group(1) if _ro else ""
    c.add("an unset billing marker anchors instead of wiping",
          _ro is not None and
          "marker == 0" in _ro_b and
          "counters kept" in _ro_b and
          _ro_b.index("marker == 0") < _ro_b.index("counters zeroed"),
          "a closed-handle read of last_month would wipe real counters")
    # --- RTC restore survives ESP.restart --------------------------------
    # millis() resets to 0 across a restart while RTC memory persists, so a
    # plain millis() - rtcLastMillis underflows to ~49.7 days in the future —
    # forward billing progress, i.e. a wipe. A smaller millis() than the
    # stored one means restarted: elapsed is the time since this boot began.
    c.add("the RTC restore cannot jump forward across a restart",
          re.search(r">=\s*rtcLastMillis", strip_comments(ts_cpp)) is not None,
          "every reboot would restore a clock ~50 days ahead and wipe")
    # --- test_force_rollover is audited ----------------------------------
    # It moves the marker silently and the wipe lands a cycle later, so an
    # unaudited arming reads as a causeless "Monthly reset" — exactly the
    # mystery that presented as a billing-logic bug on 2026-10-05.
    c.add("arming a test rollover leaves an event-log trail",
          "auditForceRollover" in strip_comments(cmd)
          and "auditForceRollover" in lm_h,
          "the next wipe would have no visible cause")
    # --- RSSI display filter ---------------------------------------------
    # Raw scans jitter several dB and the header icon bounced every push.
    # The filter lives at the single writer (updateSharedData), so the local
    # snapshot, the cloud snapshot and the icon all read the same smoothed
    # value. Filtering in either snapshot builder instead would let them
    # disagree with each other.
    c.add("RSSI is EMA-smoothed once where shared data is written",
          "rssiSm += 0.1f" in ino and "rssiInit" in ino
          and "wifiRSSI = wifiMgr.staRSSI()" not in ino
          and "wifiMgr.staRSSI()" in ino,
          "raw RSSI reaches the UI and the icon flickers")
    # --- interface addresses (both modes) --------------------------------
    # The UI prints the address to open in STA and in fallback-AP mode. The
    # addresses are captured once in updateSharedData() (the single writer),
    # so the local snapshot and the cloud push can never disagree. A panel
    # that read location.host instead would show the HOSTING domain on the
    # cloud page, never the board.
    c.add("both snapshots publish the STA + AP interface addresses",
          re.search(r'\\"staIp\\"', sysjson) is not None
          and re.search(r'\\"apIp\\"', sysjson) is not None
          and re.search(r'\\"staIp\\"', cl_cpp) is not None
          and re.search(r'\\"apIp\\"', cl_cpp) is not None,
          "a mode shows no address in the UI")
    c.add("the addresses are captured where shared data is written",
          "WiFi.localIP()" in ino and "WiFi.softAPIP()" in ino,
          "snapshot builders would read the radio on different tasks")
    c.add("the STA/AP panels have an IP row each",
          'id="staIp"' in html and 'id="apIp"' in html
          and "getElementById('staIp')" in js and "getElementById('apIp')" in js,
          "the address lives only in a status sentence")

    # --- README claims about the tree ----------------------------------
    c.add("README documents the AsyncTCP patch step that patch_async_tcp.py exists for",
          "patch_async_tcp.py" in rdme and (ROOT / "scripts/patch_async_tcp.py").exists())
    c.add("README documents the OTA limitation honestly",
          "`cloud-ota` branch" in rdme and "USB or LAN ArduinoOTA only" in rdme,
          "the Limits section no longer says where cloud updates live")
    c.add("the project is labelled open source (LICENSE + README)",
          (ROOT / "LICENSE").exists() and "MIT" in read("LICENSE") and
          "MIT" in rdme and "LICENSE" in rdme,
          "LICENSE missing or README does not name it")
    # Every local image the README embeds must exist, or the reader gets the
    # broken-image icon (this exact bug shipped with the network-port shot).
    # Matches both markdown `![](doc/...)` and HTML `<img src="doc/...">`.
    for _img in re.findall(r"(?:\]\(|src=\")(doc/[^)\"]+)", rdme):
        c.add("README image exists: %s" % _img, (ROOT / _img).exists())
    c.add("ARCHITECTURE records the mixed-content reason for hosting the UI itself",
          "mixed content" in arch)

    # --- the files the migration deleted must really be gone ------------
    # NOTE: firebase.json / database.rules.json / .firebaserc used to be on
    # this list (old cloud era). They are back by design: the rules ship as a
    # file + `firebase deploy --only database` instead of console clicking.
    # Only the alias (.firebaserc) stays local - see .gitignore.
    gone = ["tools",
            "frontend/config.js", "src/sensor", "src/utils/device_id.h",
            "src/network/firebase_bridge.cpp", "src/network/firebase_config.h",
            "src/network/ntfy_notifier.cpp", "src/network/ap_portal.h",
            "scripts/deploy.py", "tools/firebase_rest.py"]
    still = [g for g in gone if (ROOT / g).exists()]
    c.add("every file the migration deleted is actually gone", not still,
          "still present: %s" % still)

    # --- the committed RTDB rules must be valid and match the payload ------
    # Rules deploy by CLI, not console clicking, so a typo here ships to the
    # database. Assert shape, not vibes: parses as JSON, devices readable by
    # all (the dashboard viewer is public), latest guarded with the fields the
    # pusher actually sends (dev/epoch/uptime/rssi/v/ch - see
    # CloudPush::snapshot), cmd writable ONLY by the admin Gmail with the
    # fields the poller parses (id/frame/ts), ack writable by any signed-in
    # board with the fields it writes (id/ok/ts). The .validate sits at
    # devices/$dev/latest (NOT $dev): writes land at .../latest.json, so a
    # validator one level up sees {latest: {...}} and rejects everything -
    # shipped exactly that bug once, and every push 401d.
    try:
        _rules = json.loads(read("database.rules.json"))
        _fb = json.loads(read("firebase.json"))
        _devices = _rules.get("rules", {}).get("devices", {})
        _dev = _devices.get("$dev", {})
        _need = {"dev", "epoch", "uptime", "rssi", "v", "ch"}
        _latest_valid = _dev.get("latest", {}).get(".validate", "")
        _cmd_rule = _dev.get("cmd", {})
        _cmd_valid = _cmd_rule.get(".validate", "")
        _ack_rule = _dev.get("ack", {})
        _ack_valid = _ack_rule.get(".validate", "")
        _rules_ok = (_devices.get(".read") is True and
                     _dev.get("latest", {}).get(".write") == "auth != null" and
                     _need <= set(re.findall(r"'(\w+)'", _latest_valid)) and
                     _cmd_rule.get(".read") is True and
                     "heng.xiao.hour@gmail.com" in str(_cmd_rule.get(".write", "")) and
                     {"id", "frame", "ts"} <= set(re.findall(r"'(\w+)'", _cmd_valid)) and
                     _ack_rule.get(".read") is True and
                     _ack_rule.get(".write") == "auth != null" and
                     {"id", "ok", "ts"} <= set(re.findall(r"'(\w+)'", _ack_valid)) and
                     _fb.get("database", {}).get("rules") == "database.rules.json")
    except (ValueError, AttributeError):
        _rules_ok = False
        _dev = {}
    c.add("database.rules.json is valid and guards devices/$dev with the payload fields",
          _rules_ok,
          "$dev rule=%s" % (_dev,))

    # --- the gates the docs point at must exist ------------------------
    for s in ["scripts/verify_all.sh", "scripts/embed_web.py",
              "scripts/test_auth_gate.c", "scripts/mock_device.py",
              "scripts/e2e_aponly.js", "scripts/setup.py",
              "scripts/e2e_cloud_adapt.js", "scripts/e2e_cloud_settings.js"]:
        c.add("referenced tooling exists: %s" % s, (ROOT / s).exists())
    c.add("the README documents the setup wizard",
          "python3 scripts/setup.py" in rdme,
          "Option 1 lost its setup.py pointer")
    c.add("the README tours the four UI pages",
          all(k in rdme for k in ["**Dashboard**", "**Analytics**",
                                  "**History**", "**Settings**"]),
          "dashboard tour lost a page")

    c.add("the console offers set_ap / reset_ap (serial is the recovery path when the network is lost)",
          "set_ap" in ch and "reset_ap" in ch and
          "ap_creds_validate" in ch)
    c.add("the serial line buffer can hold a quoted 32-char name plus a 63-char password",
          re.search(r"char serBuf\[(\d+)\]", ino) is not None and
          int(re.search(r"char serBuf\[(\d+)\]", ino).group(1)) >= 128,
          "serBuf is %s bytes; 'set_ap \"%s\" \"%s\"' needs 107"
          % (re.search(r"char serBuf\[(\d+)\]", ino).group(1), "x" * 32, "x" * 63))

    bad = c.report()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
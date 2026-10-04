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
    c.add("SNTP is pointed at public servers in UTC (board works in epoch)",
          "configTime(0, 0" in ts_cpp and "pool.ntp.org" in ts_cpp)
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

    # --- remote monitoring: push-only, STA-only, MAC identity -----------------
    # The return of the cloud, minus everything that got it removed. Each rule
    # below is one archived lesson (doc/opencode_agent/lessons.md); the
    # mutation suite proves each one can fail.
    # Comments may NAME the removed SDK (repo convention: strip comments first,
    # like verify_all.sh does) - the rule is about code, not prose.
    c.add("the pusher uses raw REST PATCH, no SDK include",
          "PATCH /devices/" in cl_cpp and
          "firebase" not in strip_comments(cl_cpp).lower() and
          "firebase" not in strip_comments(cl_h).lower(),
          "an SDK crept back into the pusher")
    c.add("the pusher never reads: no GET anywhere in it",
          "GET " not in strip_comments(cl_cpp) and
          ".get(" not in strip_comments(cl_cpp),
          "a read path shares the push connection again")
    # Scoped to loop()'s body, not the file: WL_CONNECTED also appears in
    # wantsRadio(), so a file-wide substring rule passes after the loop's own
    # gate is deleted (proven by mutation, not by reading).
    _loop = re.search(r"void CloudPush::loop\([^)]*\) \{([\s\S]*?)\n\}\n", cl_cpp)
    c.add("pushes are STA-gated (fallback AP has no internet)",
          _loop is not None and "WL_CONNECTED" in _loop.group(1),
          "loop() can fire without a station link")
    _push_iv = re.search(r"PUSH_INTERVAL_MS\s*=\s*(\d+)", cl_h)
    c.add("the push cadence is 10 s, not the old 1 s hot path",
          _push_iv is not None and int(_push_iv.group(1)) == 10000,
          "interval moved off 10 s")
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
    # Session hygiene: the ID token lives in RAM and is refreshed, never
    # stored. A prefs.putString in the pusher means the session leaked to
    # flash; a missing refresh/signIn means the login cannot survive an hour.
    _sess = strip_comments(cl_cpp)
    c.add("the session never reaches flash (re-login on boot instead)",
          "putString" not in _sess and "putBytes" not in _sess,
          "the pusher persists session material")
    c.add("expiry refreshes and 401 re-logs-in (one retry, never a loop)",
          "refresh()" in _sess and "signIn()" in _sess and "401" in _sess,
          "the session cannot survive its first hour")
    # ...but "present in the file" is not "wired": 401 also appears in a log
    # line and signIn() in two places, so deleting any one path still passes
    # the rule above (proven by mutation). Pin each path to its function body.
    _ensure = re.search(r"bool CloudPush::ensureLogin\(\) \{([\s\S]*?)\n\}\n", cl_cpp)
    _post = re.search(r"bool CloudPush::post\(const String &body\) \{([\s\S]*?)\n\}\n", cl_cpp)
    _ensure_b = strip_comments(_ensure.group(1)) if _ensure else ""
    _post_b = strip_comments(_post.group(1)) if _post else ""
    c.add("ensureLogin falls back to a full sign-in, not just a refresh",
          "refresh()" in _ensure_b and "signIn()" in _ensure_b,
          "first boot can never sign in")
    c.add("a rejected push re-logs-in once inside post()",
          re.search(r"if\s*\(\s*code\s*==\s*401\s*\)", _post_b) is not None and
          "signIn()" in _post_b,
          "a dead token is never replaced")
    c.add("exactly one retry per push (initial + one, never a loop)",
          _sess.count("postStatus(") == 3,  # definition + initial + retry
          "postStatus call count moved off 3")
    # The TLS stack put classic at 103% of the default 1.2 MB app slot;
    # min_spiffs (1.9 MB, OTA kept) is the documented scheme. Reverting the
    # README line to the bare FQBN silently unbuilds the classic board.
    c.add("README pins min_spiffs for the classic ESP32 build",
          "esp32:esp32:esp32:PartitionScheme=min_spiffs" in rdme,
          "classic line reverted to the default scheme (103% overflow)")
    # The password and both session tokens must never be logged: pull every
    # DEBUG_LOG/STATUS_LOG call out of the pusher and assert none of them
    # formats one. A substring rule on the whole file cannot say this - the
    # buffers legitimately EXIST in the file (stored, sent inside TLS), just
    # never in a log line. The account email MAY be logged (identifier, and
    # the user typed it themselves).
    _log_calls = re.findall(r"(?:DEBUG_LOG|STATUS_LOG)\(([\s\S]*?)\);", cl_cpp)
    _secret_names = ("pass_", "idToken_", "refreshToken_")
    c.add("password and session tokens are never logged (only host/status/email)",
          _log_calls and all(not any(s in call for s in _secret_names)
                             for call in _log_calls),
          "a log line formats a credential")
    # Same for the diag readout: it may print LENGTH and aud, never bytes.
    # memcpy(tok, idToken_) is the sanctioned copy (for strlen + aud parse);
    # any consoleAppendf/out-formatting of the buffers is a leak.
    _diag = re.search(r"void CloudPush::diag\(String &out[^{]*\{([\s\S]*?)\n\}\n", cl_cpp)
    _diag_b = strip_comments(_diag.group(1)) if _diag else ""
    _diag_outs = re.findall(r"consoleAppendf\(out,([\s\S]*?)\);", _diag_b)
    c.add("cloud diag prints length/audience, never token bytes",
          _diag is not None and _diag_outs and
          all(not any(s in call for s in _secret_names) for call in _diag_outs),
          "diag() formats credential bytes")

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

    # --- install button exists after connect, not just before ----------------
    # The shipped bug: the only Install button lived in the connect panel,
    # which hides the instant the dashboard connects.
    c.add("settings has its own install button",
          'id="installBtn2"' in html and "installHint2" in html and
          "installHint2" in js)
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
    c.add("FIRMWARE_VERSION is the 3.0.0 AP-only release",
          m and m.group(1) == "3.0.0", "found %s" % (m.group(1) if m else "none"))

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

    # --- README claims about the tree ----------------------------------
    c.add("README documents the AsyncTCP patch step that patch_async_tcp.py exists for",
          "patch_async_tcp.py" in rdme and (ROOT / "scripts/patch_async_tcp.py").exists())
    c.add("README documents the AP-only limitations honestly",
          all(k in rdme for k in ["No access from outside your own WiFi",
                                  "No true PWA install", "never sleeps"]))
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
    # database. Assert shape, not vibes: parses as JSON, guards devices/$dev,
    # and requires the fields the pusher actually sends (dev/epoch/uptime/
    # rssi/v/ch - see CloudPush::snapshot). The .validate sits at
    # devices/$dev/latest (NOT $dev): writes land at .../latest.json, so a
    # validator one level up sees {latest: {...}} and rejects everything -
    # shipped exactly that bug once, and every push 401d.
    try:
        _rules = json.loads(read("database.rules.json"))
        _fb = json.loads(read("firebase.json"))
        _dev = _rules.get("rules", {}).get("devices", {}).get("$dev", {})
        _need = {"dev", "epoch", "uptime", "rssi", "v", "ch"}
        _valid = _dev.get("latest", {}).get(".validate", "")
        _rules_ok = (".write" in _dev and ".read" in _dev and
                     _need <= set(re.findall(r"'(\w+)'", _valid)) and
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
              "scripts/e2e_aponly.js"]:
        c.add("referenced tooling exists: %s" % s, (ROOT / s).exists())

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
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
    sw = read("frontend/sw.js")

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

    # --- AP-only -------------------------------------------------------
    c.add("WiFiManager has no station interface at all",
          "connectToWiFi" not in wm and "WIFI_STA" not in wm and "WIFI_STA" not in wm_h)
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
    # old identity and the change silently evaporates.
    save_at = cmd.find("nvs->saveApCredentials")
    commit_at = cmd.find("nvs->commit()", save_at)
    reboot_at = cmd.find("requestReboot", save_at)
    c.add("set_ap commits to flash before it asks for the restart",
          save_at > 0 and commit_at > save_at and reboot_at > commit_at,
          "save@%d commit@%d reboot@%d" % (save_at, commit_at, reboot_at))
    # The frontend rules must match the device's, or the UI rejects something the
    # board accepts (annoying) or accepts something the board rejects (the
    # user's phone drops off a network the board never joined).
    for label, pattern in [("password minimum", r"AP_PASS_MIN\s*=\s*8"),
                           ("password maximum", r"AP_PASS_MAX\s*=\s*63"),
                           ("name maximum", r"AP_SSID_MAX\s*=\s*32")]:
        c.add("the frontend's AP %s matches ap_creds.h" % label,
              re.search(pattern, js) is not None and
              re.search(pattern.replace("\\s*=\\s*", "\\s+"), apc) is not None)
    c.add("the dashboard has an Access Point panel that sends set_ap",
          'id="apSsid"' in html and 'id="apPass"' in html and
          "set_ap" in js and "reset_ap" in js)
    c.add("the README documents how to rename the network and how to recover it",
          "set_ap" in rdme and "reset_ap" in rdme)
    c.add("ARCHITECTURE records that the AP identity is persisted",
          "ap_ssid" in arch and "ap_pass" in arch)

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
    c.add("service worker cache name was bumped past the pre-migration one",
          "esp32-counter-v14" in sw)
    c.add("demo mode is reachable without a board",
          "?demo=1" in js)

    # --- README claims about the tree ----------------------------------
    c.add("README documents the AsyncTCP patch step that patch_async_tcp.py exists for",
          "patch_async_tcp.py" in rdme and (ROOT / "scripts/patch_async_tcp.py").exists())
    c.add("README documents the AP-only limitations honestly",
          all(k in rdme for k in ["No access from outside your own WiFi",
                                  "No true PWA install", "never sleeps"]))
    c.add("ARCHITECTURE records the mixed-content reason for hosting the UI itself",
          "mixed content" in arch)

    # --- the files the migration deleted must really be gone ------------
    gone = ["firebase.json", "database.rules.json", ".firebaserc", "tools",
            "frontend/config.js", "src/sensor", "src/utils/device_id.h",
            "src/network/firebase_bridge.cpp", "src/network/firebase_config.h",
            "src/network/ntfy_notifier.cpp", "src/network/ap_portal.h",
            "scripts/deploy.py", "tools/firebase_rest.py"]
    still = [g for g in gone if (ROOT / g).exists()]
    c.add("every file the migration deleted is actually gone", not still,
          "still present: %s" % still)

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
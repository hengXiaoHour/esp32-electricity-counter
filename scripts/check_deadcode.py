#!/usr/bin/env python3
"""Fail if dead code or dead assets reappear.

The AP-only cleanup removed 9 never-called methods, an unused constant, 9 dead
CSS rules, a dead JS function and a block of permanently-hidden markup. Nothing
stops all of that coming back except a check, so this is the check.

Each rule below is deliberately written to avoid the false positives that made
earlier ad-hoc versions of this audit useless:

  * Comments are stripped before matching. An earlier version reported "the
    firmware still calls the deprecated beginResponse_P" when the only match was
    a comment explaining that it does not.
  * A qualified call `Class::method(` is a USE, not a definition. Counting those
    as definitions made `serveAsset` and `defaultChannelName` look unused when
    they are called exactly once each.
  * `->method()` and `.method()` are uses. A lookbehind that excluded `>`
    hid every single NVS call.
  * CSS class matching is token-bounded. `.org` and `.w3` "matched" inside the
    `www.w3.org` string of an inline SVG data-URI.
  * Functions referenced from an HTML onclick/inline handler count as used.

Usage:  python3 scripts/check_deadcode.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


def strip_cpp(t):
    t = re.sub(r"/\*.*?\*/", " ", t, flags=re.S)
    t = re.sub(r"//[^\n]*", " ", t)
    return t


def strip_css(t):
    return re.sub(r"/\*.*?\*/", " ", t, flags=re.S)


def strip_js(t):
    t = re.sub(r"/\*.*?\*/", " ", t, flags=re.S)
    t = re.sub(r"^\s*//.*$", "", t, flags=re.M)
    return t


class R:
    def __init__(self):
        self.items = []

    def add(self, name, ok, detail=""):
        self.items.append((name, bool(ok), detail))

    def out(self):
        bad = 0
        for n, ok, d in self.items:
            if ok:
                print("  ok   %s" % n)
            else:
                bad += 1
                print("  FAIL %s%s" % (n, ("  [%s]" % d) if d else ""))
        print("\n%d dead-code rules, %d violated" % (len(self.items), bad))
        return bad


def main():
    r = R()
    # web_assets.h is GENERATED and contains the dashboard's HTML/CSS/JS inside
    # a raw string literal. Scanning it as C++ finds JS functions and CSS
    # functions ("rgba", "showInstallRow", "var") and reports them as dead C++
    # methods. It is excluded here and covered by the frontend rules instead.
    GENERATED = {"src/network/web_assets.h"}
    cpp = [p for p in ROOT.rglob("*")
           if p.suffix in (".cpp", ".h", ".ino")
           and ".git" not in p.parts and "build" not in p.parts
           and str(p.relative_to(ROOT)) not in GENERATED]
    cpp_txt = {str(p.relative_to(ROOT)): strip_cpp(p.read_text(errors="replace")) for p in cpp}
    bodies = "\n".join(t for f, t in cpp_txt.items() if not f.endswith(".h"))

    # --- 1. header-declared methods that nothing ever calls -------------
    declared = {}
    for f, t in cpp_txt.items():
        if not f.endswith(".h"):
            continue
        for m in re.finditer(
                r"^\s{2}(?:static\s+)?(?:virtual\s+)?[A-Za-z_][\w:<>\s\*&]*?"
                r"\b([a-z_]\w*)\s*\([^;{]*\)\s*(?:const\s*)?;", t, re.M):
            declared.setdefault(m.group(1), f)
    dead = []
    for name, where in sorted(declared.items()):
        if name in ("begin", "loop", "setup"):
            continue
        total = len(re.findall(r"(?<![\w])" + re.escape(name) + r"\s*\(", bodies))
        # A DEFINITION is `Class::method(...)` followed by `{`. A qualified CALL
        # is `Class::method(...)` with no body. Counting both as definitions made
        # serveAsset (called once, inside the onNotFound lambda) and
        # defaultChannelName (called once, in the .ino) look dead when they are
        # not - which is how a checker learns to be ignored.
        defs = len(re.findall(r"::" + re.escape(name) + r"\s*\([^;{]*\)\s*(?:const\s*)?\{",
                              bodies, re.S))
        if total - defs <= 0:
            dead.append("%s (%s)" % (name, where))
    r.add("every header-declared method is called from a .cpp/.ino",
          not dead, "never called: " + ", ".join(dead))

    # --- 2. config.h constants nobody reads ------------------------------
    cfg = read("src/config.h")
    consts = re.findall(r"^#define\s+([A-Z_][A-Z0-9_]*)\b", cfg, re.M)
    # Search config.h too, but ignore the constant's own `#define` line. The six
    # PIN_CURRENT_CH* are only referenced by the CURRENT_PINS array that lives in
    # config.h itself, so excluding the whole file wrongly reported them dead.
    defs_lines = {c: "#define %s" % c for c in consts}
    unused = []
    for c in consts:
        pat = re.compile(r"(?<![A-Za-z0-9_])" + c + r"(?![A-Za-z0-9_])")
        hits = [f for f, t in cpp_txt.items()
                if any(pat.search(ln) and defs_lines[c] not in ln for ln in t.split("\n"))]
        if not hits:
            unused.append(c)
    r.add("every config.h constant is referenced somewhere", not unused,
          "unused: " + ", ".join(unused))

    # --- 3. dead CSS classes ---------------------------------------------
    html, css, js, sw = read("frontend/index.html"), read("frontend/style.css"), \
        read("frontend/script.js"), read("frontend/sw.js")
    css_body = strip_css(css)
    # Drop data-URI payloads before harvesting class names: `www.w3.org` in an
    # inline SVG otherwise yields phantom classes `.org` and `.w3`.
    # `[^"')]*` cannot work here: the inline SVG data-URIs contain their own
    # quotes. The payload has no ')' in it, so match up to the first ')'.
    css_body = re.sub(r"url\([^)]*\)", " ", css_body)
    referenced = html + js + sw
    classes = set(re.findall(r"\.([a-zA-Z][\w-]*)", css_body))
    dead_css = sorted(
        c for c in classes
        if not re.search(r"(?<![\w-])" + re.escape(c) + r"(?![\w-])", referenced))
    r.add("no CSS class is defined but never used", not dead_css,
          "unused: " + ", ".join(".%s" % c for c in dead_css))

    # --- 3b. no form control falls through to the browser default ---------
    # The Admin PIN inputs rendered as white boxes on the dark theme because
    # the stylesheet selected `input[type="text"], input[type="number"]` and
    # both PIN fields are type="password". Selecting by type is the bug: the
    # next new field is another type, or the same one in a row that was not
    # wrapped in a .cal-row-single. So every control must either carry a
    # themed class of its own or sit inside a themed row, and the stylesheet
    # must style inputs by ELEMENT (.cal-row-single input), never by type.
    THEMED_ROWS = ("cal-row-single", "cal-param-row", "form-group", "console-row",
                   "cal-field")
    THEMED_CLASSES = ("console-input",)
    unthemed = []
    for m in re.finditer(r"<(input|select)\b([^>]*)>", html, re.S):
        tag, attrs = m.group(1), m.group(2)
        ident = re.search(r'id="([^"]+)"', attrs)
        cls = re.search(r'class="([^"]*)"', attrs)
        cls_tokens = set(cls.group(1).split()) if cls else set()
        # The parent is the tag before this one that is not self-closed.
        start = html.rfind("<", 0, m.start())
        parent_tag = re.match(r"<([a-zA-Z][\w-]*)", html[start:start + 40])
        parent = parent_tag.group(1) if parent_tag else ""
        # Walk up two levels of closing/opening tags is overkill; the themed
        # rows all put the control directly inside the row element, so the
        # nearest enclosing element name is enough when it is one of them.
        near = html[max(0, m.start() - 400):m.start()]
        parent_cls = ""
        last = None
        for om in re.finditer(r'<div\b([^>]*)>', near):
            last = om
        if last:
            c = re.search(r'class="([^"]*)"', last.group(1))
            parent_cls = c.group(1) if c else ""
        if tag == "input" and 'type="checkbox"' in attrs:
            continue
        ok = (cls_tokens & set(THEMED_CLASSES)) or (parent in THEMED_ROWS) \
            or (parent_cls in THEMED_ROWS)
        if not ok:
            unthemed.append(tag + "#" + (ident.group(1) if ident else "(no id)"))
    r.add("every input/select is inside a themed form row", not unthemed,
          "browser-default styling: " + ", ".join(unthemed))

    # Guard the fix rather than the symptom: an input rule written by type is
    # what let password fields slip through in the first place. Checkboxes are
    # the one legitimate exception - they are a genuinely different control.
    typed_input_rules = [m.group(0) for m in
                         re.finditer(r"input\s*\[\s*type\s*[~^|$*]?=([^\]]*)\]", css_body)
                         if "checkbox" not in m.group(1)]
    r.add("no input CSS rule filters on type= (checkboxes excepted)",
          not typed_input_rules,
          "found %d: %s" % (len(typed_input_rules), ", ".join(typed_input_rules)))

    # --- 3c. index.html and sw.js must agree on the asset version ---------
    # They drifted to style.css?v=...a vs ?v=...b, which silently pre-cached a
    # URL the page never requests, so the offline fallback could never hit.
    html_ver = dict(re.findall(r'(style\.css|script\.js)\?v=([\w.]+)', html))
    sw_ver = dict(re.findall(r"'\./(style\.css|script\.js)\?v=([\w.]+)'", sw))
    r.add("index.html and sw.js request the same asset versions",
          html_ver and html_ver == sw_ver,
          "index.html=%s sw.js=%s" % (html_ver, sw_ver))

    # --- 4. dead JS functions --------------------------------------------
    js_body = strip_js(js)
    # HTML ONLY. Including js_body here double-counts each function's own
    # declaration, so used_html was never 0 and this rule never fired - proven by
    # a mutant function appended to a real copy of script.js and still reported
    # "no JS function is declared but never called".
    inline_handlers = html
    dead_js = []
    for m in re.finditer(r"^\s*(?:async\s+)?function\s+([A-Za-z_$][\w$]*)", js_body, re.M):
        fn = m.group(1)
        used_js = len(re.findall(r"(?<![\w$.])" + re.escape(fn) + r"\s*\(", js_body))
        used_html = len(re.findall(r"\b" + re.escape(fn) + r"\s*\(", inline_handlers))
        if used_js <= 1 and used_html == 0:
            dead_js.append(fn)
    r.add("no JS function is declared but never called", not dead_js,
          "unused: " + ", ".join(dead_js))

    # --- 5. markup that can never become visible ------------------------
    # An element that is `hidden` in the HTML and never un-hidden by script is
    # permanently invisible scaffolding. This is exactly what the Google-auth
    # sidebar block became once applyAuthState() was deleted.
    hidden_ids = re.findall(r'id="([\w-]+)"[^>]*class="[^"]*\bhidden\b', html)
    dead_hidden = [i for i in hidden_ids
                   if ("'%s'" % i) not in js and ('"%s"' % i) not in js
                   and not re.search(r"(?<![\w-])" + re.escape(i) + r"(?![\w-])", css)]
    r.add("no element is permanently hidden markup", not dead_hidden,
          "hidden and never toggled: " + ", ".join("#" + i for i in dead_hidden))

    # --- 6. the files this cleanup deleted must stay deleted -------------
    # NOTE: firebase.json / database.rules.json / .firebaserc used to be here
    # (old cloud era). They are back by design - see check_docs.py. Only the
    # alias (.firebaserc) stays local via .gitignore.
    # NOTE: scripts/setup.py is back too, as a DIFFERENT tool: the old one was
    # the service-account-era env wizard (firebase_config.h, frontend/config.js,
    # Arduino lib Dadoinstaller); the new one provisions a board over serial
    # (setwifi/setcloud). The path is pinned by content below, not by name.
    gone = ["scripts/deploy.py", "frontend/config.js",
            "frontend/config.example.js", "doc/opencode_agent/memories.json",
            ".workflow/active.json", ".workflow/PLAN.md", ".workflow/RESEARCH.md",
            "tools",
            "src/sensor", "src/utils/device_id.h", "src/utils/device_id.cpp",
            "src/network/firebase_bridge.cpp", "src/network/firebase_config.h",
            "src/network/ntfy_notifier.cpp", "src/network/ap_portal.h",
            "scripts/__pycache__", "opencode.json", ".workflow/VERIFICATION.log",
            "doc/esp32s3-electricity-counter-prompt.md"]
    # NOTE: the .workflow DIRECTORY itself is deliberately not in this list. It is
    # legitimate per-task scratch (PLAN.md / RESEARCH.md / active.json, all
    # gitignored) and is expected to reappear while a task is in flight. Only the
    # hand-maintained changelog it used to hold is pinned as gone.
    # NOTE: build/ is deliberately NOT in this list. arduino-cli's default build
    # directory IS ./build, so it is recreated by any plain `arduino-cli compile`
    # with no --output-dir. It is gitignored and transient; treating it as junk
    # to be kept deleted would just make the gate noisy. Pass --output-dir.
    back = [g for g in gone if (ROOT / g).exists()]
    r.add("the cleanup deletions stay deleted", not back, "reappeared: " + ", ".join(back))

    # --- 6b. the AP identity the radio broadcasts is the one loaded from NVS
    # `set_ap` saved the new name, the board rebooted, and it came back up
    # beaconing the factory name anyway. The cause: loadCredentials() wrote
    # apSsid_/apPass_, and softAP() / getSSID() / getPass() all read the
    # separate AP_SSID/AP_PASS default pointers instead, so the loaded value
    # was written and never used.
    #
    # Nothing caught that for weeks: the save worked, the console said
    # "Saved", and only rebooting the board and watching an actual WiFi scan
    # showed the truth. So the check is on the SHAPE of the read, not on a log
    # line. Scoped to the right functions: a bare "apSsid_ appears in
    # wifi_manager.cpp" would also have passed against the broken code, which is
    # precisely the mistake that let it ship.
    def body_of(text, sig):
        """Brace-matched body of `sig` in `text`, or '' if not found.

        A substring assertion is not enough here: wifi_manager.cpp contains
        several softAP-looking fragments, only one of which is the real call.
        """
        m = re.search(re.escape(sig) + r"[^\{;]*\{", text)
        if not m:
            return ""
        i = m.end() - 1
        depth = 0
        for j in range(i, len(text)):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    return text[i:j + 1]
        return ""

    wcpp = strip_cpp(read("src/network/wifi_manager.cpp"))
    wch = strip_cpp(read("src/network/wifi_manager.h"))
    ap_body = body_of(wcpp, "void WiFiManager::startFallbackAP")
    reads_buffer = bool(ap_body) and "apSsid_" in ap_body
    getter_ssid = body_of(wch, "getSSID")
    getter_pass = body_of(wch, "getPass")
    r.add("softAP() broadcasts the credentials loaded from NVS, not the defaults",
          reads_buffer and "softAP(" in ap_body,
          "startAPMode does not use apSsid_" if not reads_buffer else "startAPMode not found")
    r.add("getSSID()/getPass() return the live buffers",
          "apSsid_" in getter_ssid and "apPass_" in getter_pass,
          "getSSID->%r getPass->%r" % (getter_ssid.strip(), getter_pass.strip()))

    # --- 6b. STA-first: the AP must stay OFF unless the home link fails ----
    # The exact bugs this catches: startStaMode() that never calls WiFi.begin(),
    # a fallback that never starts the AP, an isReady() that only holds in one
    # state (dashboard never starts in the other), or a placeholder guard that
    # checks the compiled default instead of the runtime buffer (real NVS
    # credentials then sit quiet and never join - shipped once, caught here).
    sta_body = body_of(wcpp, "void WiFiManager::startStaMode")
    fb_body = body_of(wcpp, "void WiFiManager::startFallbackAP")
    r.add("startStaMode starts the station link from the live buffer",
          bool(sta_body) and "WiFi.begin(" in sta_body and "staSsid_" in sta_body,
          "no WiFi.begin(staSsid_) inside startStaMode")
    r.add("startStaMode runs STA-only, never starting the AP itself",
          bool(sta_body) and "WIFI_STA" in sta_body and "softAP(" not in sta_body,
          "startStaMode touches the AP")
    r.add("the placeholder guard compares the runtime buffer, not the default",
          "strcmp(staSsid_, STA_SSID_PLACEHOLDER)" in (sta_body + fb_body),
          "guard checks the default instead of staSsid_")
    r.add("the fallback starts the AP for recovery",
          bool(fb_body) and "softAP(" in fb_body and "apSsid_" in fb_body,
          "startFallbackAP does not broadcast apSsid_")
    r.add("isReady() holds in EITHER running state, never in INIT",
          "WIFI_STA_MODE" in strip_cpp(read("src/network/wifi_manager.h")) and
          "WIFI_AP_MODE" in strip_cpp(read("src/network/wifi_manager.h")) and
          "WIFI_INIT" not in
          re.search(r"bool isReady\(\) const \{[^}]*\}",
                    read("src/network/wifi_manager.h")).group(0),
          "isReady() misses a running state or admits INIT")
    # stationUp()/staRSSI() are read by the .ino and the console; a dead
    # getter is the same silent-nothing class of bug.
    ino_txt = read("esp32-electricity-counter.ino")
    r.add("the station getters are actually called by firmware and console",
          "wifiMgr.stationUp()" in ino_txt and
          "stationUp()" in strip_cpp(read("src/network/console_handler.cpp")),
          "stationUp()/staRSSI() have no callers")
    # The two default pointers must stay gone: leaving them declared invites
    # the same read-by-accident, and nothing else references them now.
    stale_ptr = [sym for sym in ("AP_SSID", "AP_PASS")
                 if re.search(r"(?<![\w_])WiFiManager::" + sym + r"(?![\w_])",
                              read("src/network/wifi_manager.cpp"))
                 or re.search(r"static\s+const\s+char\s*\*\s*" + sym + r"\s*;", wch)]
    r.add("the dead AP_SSID/AP_PASS default pointers stay deleted", not stale_ptr,
          "reappeared: " + ", ".join(stale_ptr))

    # --- 7. the generated header must stay out of git -------------------
    # src/network/web_assets.h is 151 KB derived from frontend/. Committing it
    # put a fifth of the working tree in git and, because each migration phase
    # rewrote it, ~940 KB of near-duplicate blobs in history.
    import subprocess
    tracked = subprocess.run(
        ["git", "ls-files", "--error-unmatch", "src/network/web_assets.h"],
        cwd=str(ROOT), capture_output=True, text=True)
    r.add("the generated web_assets.h stays untracked", tracked.returncode != 0,
          "it is committed again - untrack with "
          "'git rm --cached src/network/web_assets.h'")

    # --- 7b. no credential material is tracked, by name or by content -----
    # Oct 2026: flipping this repo public exposed two service-account key
    # files that `git add -A` had swept up in Sep 2026 (removed from HEAD
    # days later, but the blobs stayed in history). Google disabled the keys
    # within the hour and paused the project. The Sep-2026 fix HAD added a
    # *firebase-adminsdk*.json ignore, but a later rewrite of .gitignore
    # silently dropped it - so this rule asserts the guard two independent
    # ways: the ignore pattern must exist in .gitignore AND no tracked file
    # may match the name or hold a PEM block. Either half catches what the
    # other misses (a renamed key file, or a key pasted into an innocent
    # filename).
    _tracked = subprocess.run(
        ["git", "ls-files"], cwd=str(ROOT), capture_output=True,
        text=True).stdout.splitlines()
    _key_files = [f for f in _tracked if "firebase-adminsdk" in f]
    r.add("no service-account key file is tracked", not _key_files,
          "tracked: " + ", ".join(_key_files))
    _leaky = []
    # The needle is built at runtime: written as one literal, this file would
    # match itself and the gate would fail on clean code.
    _pem = "BEGIN PRIVATE" + " KEY"
    for _f in _tracked:
        try:
            _p = ROOT / _f
            if _p.stat().st_size > 2000000:
                continue
            if _pem in _p.read_text(encoding="utf-8",
                                errors="replace"):
                _leaky.append(_f)
        except OSError:
            pass
    r.add("no tracked file holds private-key material", not _leaky,
          "key block in: " + ", ".join(_leaky))
    r.add("the key-file ignore pattern is present in .gitignore",
          "*firebase-adminsdk*.json" in read(".gitignore"),
          "a .gitignore rewrite dropped the guard again")

    # --- 8. the AP rename path must stay wired end to end -----------------
    # Not a dead-code rule in the usual sense, but the same disease seen from the
    # other side: every layer of this feature can be removed one at a time and
    # each removal individually looks like tidying. Delete the WebSocket verb and
    # the dashboard button silently becomes a no-op; delete the console verb and
    # "I forgot the password" becomes impossible; delete the validation and the
    # board bricks its own radio. Assert the chain.
    cmd = strip_cpp(read("src/network/command_processor.cpp"))
    ch = strip_cpp(read("src/network/console_handler.cpp"))
    nm = strip_cpp(read("src/utils/nvs_manager.cpp"))

    # Two traps here, both found by mutation testing rather than by reading:
    #   * substrings - "saveApCredentials" is a prefix of "saveApCredentialsX",
    #     so renaming the definition left the check passing;
    #   * COMMENTS - the file's header comment lists the whole verb vocabulary,
    #     including set_ap, so a plain search matched the documentation of the
    #     verb rather than the verb. Comments are stripped for that reason.
    def has(word, hay):
        return re.search(r"(?<![A-Za-z0-9_])" + re.escape(word) + r"(?![A-Za-z0-9_])",
                         hay) is not None

    ap_missing = []
    for what, present in [
            ("firmware verb (set_ap)", has("set_ap", cmd)),
            ("firmware verb (reset_ap)", has("reset_ap", cmd)),
            ("console verb (set_ap)", has("set_ap", ch)),
            ("console verb (reset_ap)", has("reset_ap", ch)),
            ("console help lists set_ap", "set_ap <name> <pw>" in ch),
            ("NVS writer", has("saveApCredentials", nm)),
            ("NVS eraser", has("clearApCredentials", nm)),
            ("dashboard save", "cmd: 'set_ap'" in js),
            ("dashboard reset", "cmd: 'reset_ap'" in js),
            ("dashboard panel", 'id="apSsid"' in read("frontend/index.html"))]:
        if not present:
            ap_missing.append(what)
    r.add("the AP rename feature is still wired end to end", not ap_missing,
          "disconnected at: " + ", ".join(ap_missing))

    # --- 8b. the cloud feature must stay wired end to end ------------------
    # Same disease as rule 8: each layer can be deleted one at a time while
    # every remaining layer looks tidy. The auth path gets one extra shape
    # check (the SDK must start from the NVS-loaded buffers, not a default
    # or constant) - the AP rename shipped exactly that bug once (saved the
    # new name, broadcast the old one), and a credential variant would push
    # to the wrong database while the UI says "Saved".
    clcpp = strip_cpp(read("src/network/cloud_push.cpp"))
    post_body = body_of(clcpp, "int CloudPush::postStatus")
    begin_body = body_of(clcpp, "void CloudPush::begin")
    cl_loop_body = body_of(clcpp, "void CloudPush::loop")
    cl_poll_body = body_of(clcpp, "void CloudPush::pollCmd")
    sdk_body = body_of(clcpp, "static void cloudSdkEnsure")
    cloud_missing = []
    for what, present in [
            ("firmware verb (setcloud)", has("setcloud", cmd)),
            ("firmware verb (clearcloud)", has("clearcloud", cmd)),
            ("console verb (setcloud)", has("setcloud", ch)),
            ("console verb (clearcloud)", has("clearcloud", ch)),
            # Dispatch-shaped, not just the word: the help STRING also contains
            # "setcloud", so has() alone passes after the dispatch is deleted
            # (proven by mutation). Match the full dispatch literal including
            # the trailing space - "setcloudX " must not satisfy it.
            ("console dispatch (setcloud)", 'startsWith("setcloud ")' in ch),
            ("console dispatch (clearcloud)", '== "clearcloud"' in ch),
            ("console help lists setcloud", "setcloud <host> <email> <pass>" in ch),
            ("NVS writer", has("saveFb", nm)),
            ("NVS eraser", has("clearFb", nm)),
            ("NVS gate", has("fbEnabled", nm)),
            ("shape validation", has("cloud_validateHost", ch)),
            ("dashboard save", "cmd: 'setcloud'" in js),
            ("dashboard forget", "cmd: 'clearcloud'" in js),
            ("dashboard panel", 'id="cloudHost"' in read("frontend/index.html")),
            ("begin loads from NVS", bool(begin_body) and "loadFb" in begin_body),
            # SDK-shaped, not word-shaped: "Firebase" also appears in comments,
            # so match the merge call on the push session with the device id
            # in the path - deleting the device path still "mentions Firebase".
            ("post merge-writes the device node (never a constant path)",
             bool(post_body) and "updateNode(" in post_body and
             "deviceId_" in post_body),
            ("SDK starts from the NVS-loaded account (never a default)",
             bool(sdk_body) and "auth.user.email" in sdk_body),
            ("SDK uses the herd API key (never a service-account key)",
             bool(sdk_body) and "CLOUD_API_KEY_DEFAULT" in sdk_body and
             "service_account" not in clcpp.lower()),
            # Downlink: same disease as above - each layer deletable while the
            # rest looks tidy. Shape-scoped, not word-scoped: has("pollCmd")
            # alone passes on the definition after the loop wiring is deleted,
            # and "saveCloudCmdId" alone passes on the NVS definition after the
            # ack path stops calling it (both proven by mutation, not reading).
            ("downlink fetch (cmd node, poll session)", "/cmd" in clcpp and
             bool(cl_poll_body) and "s_pollFbdo" in cl_poll_body),
            ("downlink ack (ack node)", "/ack" in clcpp),
            ("poll executes cloud frames",
             bool(cl_poll_body) and "processCommand(" in cl_poll_body),
            ("poll skips the PIN (cloud trusts RTDB rules, not a PIN)",
             bool(cl_poll_body) and
             re.search(r"processCommand\([\s\S]{0,400}?, true\)",
                       cl_poll_body) is not None),
            ("poll wired into loop",
             bool(cl_loop_body) and "pollCmd(" in cl_loop_body),
            ("acked id persisted (no re-run after reboot)",
             bool(cl_poll_body) and "saveCloudCmdId(" in cl_poll_body),
            ("begin restores the last acked id",
             bool(begin_body) and "loadCloudCmdId" in begin_body)]:
        if not present:
            cloud_missing.append(what)
    r.add("the cloud monitoring feature is still wired end to end", not cloud_missing,
          "disconnected at: " + ", ".join(cloud_missing))

    # --- 8c. local OTA stays LAN-only (no cloud updater) -----------------
    # The cloud updater is gone on purpose: no github download, no staged
    # link, no `ota` console verb, no Check/Update buttons. What must stay
    # is the LAN path (ArduinoOTA begin/handle) plus the progress signal
    # the LED, eco and the banner read. Each absence below is asserted as
    # well as each presence: a re-added cloud layer must fail loudly.
    ota_cpp_s = strip_cpp(read("src/network/ota_handler.cpp"))
    ota_h_s = strip_cpp(read("src/network/ota_handler.h"))
    sysjson_s = strip_cpp(read("src/network/system_json.cpp"))
    clcpp_s = strip_cpp(read("src/network/cloud_push.cpp"))
    cloudjs_s = strip_js(read("cloud-viewer/cloud.js"))
    front_html = read("frontend/index.html")
    build_sh = read("scripts/build.sh")
    import os as _os
    ota_missing = []
    for what, present in [
            ("LAN handler begins ArduinoOTA",
             "ArduinoOTA.begin()" in ota_cpp_s),
            ("LAN handler pumps ArduinoOTA",
             "ArduinoOTA.handle()" in ota_cpp_s),
            ("progress signal kept for LED/eco/banner",
             has("isInProgress", ota_h_s)),
            ("no cloud download loop", "loopCloud" not in ota_cpp_s),
            ("no staged-link validator call",
             "ota_url_validate" not in ota_cpp_s),
            ("no reboot-to-updater flag", "cloudRebootDue" not in ota_h_s),
            ("sketch has no updater reboot",
             "cloudRebootDue()" not in ino_txt and
             "consumeCloudReboot()" not in ino_txt),
            ("validator files gone",
             not _os.path.exists(ROOT / "src/network/ota_url.cpp") and
             not _os.path.exists(ROOT / "src/network/ota_url.h")),
            ("no staged NVS keys", "ota_url" not in nm and "ota_err" not in nm),
            ("no console ota verbs",
             'startsWith("ota ")' not in ch and '"ota status"' not in ch and
             "cmdOta" not in ch),
            ("local snapshot still publishes ota",
             has("otaInProgress", sysjson_s)),
            ("cloud snapshot still mirrors otaRun",
             "otaRun" in clcpp_s),
            ("cloud adapter still maps ota onto the banner",
             "otaRun" in cloudjs_s and "otaProgress" in cloudjs_s),
            ("no release checker in the viewer", "checkFirmware" not in js),
            ("no update sender in the viewer",
             "startFirmwareUpdate" not in js),
            ("no Check button", 'onclick="checkFirmware()"' not in front_html),
            ("no Update button",
             'onclick="startFirmwareUpdate()"' not in front_html),
            ("firmware panel still shows the running version",
             'id="fwRunning"' in front_html),
            ("no release API call", "api.github.com" not in js),
            ("classic build path kept", "--classic" in build_sh),
            ("version stamp kept", "--version" in build_sh)]:
        if not present:
            ota_missing.append(what)
    r.add("OTA is local-only and still wired end to end", not ota_missing,
          "broken at: " + ", ".join(ota_missing))

    # --- 9. the defaults the docs quote must be the ones the code uses ------
    # Two copies of "ESP32-Elec-Counter" exist on purpose (config.h for the
    # firmware, mock_device.py for the E2E harness) precisely because the harness
    # must not import firmware headers. That makes drift possible and silent.
    mcfg = re.search(r'AP_SSID_DEFAULT\s*=\s*"([^"]+)"', read("src/config.h"))
    mmock = re.search(r'AP_SSID_DEFAULT\s*=\s*"([^"]+)"', read("scripts/mock_device.py"))
    r.add("the mock board's factory SSID matches src/config.h",
          mcfg and mmock and mcfg.group(1) == mmock.group(1),
          "config.h=%s mock=%s" % (mcfg.group(1) if mcfg else "?",
                                   mmock.group(1) if mmock else "?"))

    bad = r.out()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
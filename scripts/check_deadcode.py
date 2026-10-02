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
    gone = ["scripts/setup.py", "scripts/deploy.py", "frontend/config.js",
            "frontend/config.example.js", "doc/opencode_agent/memories.json",
            ".workflow/active.json", ".workflow/PLAN.md", ".workflow/RESEARCH.md",
            "firebase.json", "database.rules.json", ".firebaserc", "tools",
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

    # --- 8. the AP rename path must stay wired end to end -----------------
    # Not a dead-code rule in the usual sense, but the same disease seen from the
    # other side: every layer of this feature can be removed one at a time and
    # each removal individually looks like tidying. Delete the WebSocket verb and
    # the dashboard button silently becomes a no-op; delete the console verb and
    # "I forgot the password" becomes impossible; delete the validation and the
    # board bricks its own radio. Assert the chain.
    cmd = read("src/network/command_processor.cpp")
    ch = read("src/network/console_handler.cpp")
    nm = read("src/utils/nvs_manager.cpp")
    ap_missing = []
    for what, needle, hay in [
            ("firmware verb (set_ap)", '"cmd\\":\\"set_ap\\""', cmd),
            ("firmware verb (reset_ap)", '"cmd\\":\\"reset_ap\\""', cmd),
            ("console verb (set_ap)", "set_ap ", ch),
            ("console verb (reset_ap)", "reset_ap", ch),
            ("NVS writer", "saveApCredentials", nm),
            ("NVS eraser", "clearApCredentials", nm),
            ("dashboard save", "cmd: 'set_ap'", js),
            ("dashboard reset", "cmd: 'reset_ap'", js),
            ("dashboard panel", 'id="apSsid"', read("frontend/index.html"))]:
        if needle not in hay:
            ap_missing.append(what)
    r.add("the AP rename feature is still wired end to end", not ap_missing,
          "disconnected at: " + ", ".join(ap_missing))

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
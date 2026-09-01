#!/usr/bin/env python3
"""
scripts/setup.py — one-shot setup for esp32-electricity-counter.

Automates the whole first-boot setup:
  1. Fill gitignored configs step-by-step (frontend/config.js, src/network/firebase_config.h)
  2. Check Python deps (cryptography for tools/hosting_deploy.py)
  3. Check Arduino CLI + ESP32 core + required libraries
  4. Check Firebase CLI / REST deploy path
  5. Optionally compile firmware (arduino-cli)

Usage:
  python3 scripts/setup.py              # interactive wizard + auto-install Arduino libs/cores
  python3 scripts/setup.py --check      # only check, no writes / no install
  python3 scripts/setup.py --no-install # skip Arduino install even if missing
  python3 scripts/setup.py --compile    # + compile firmware after setup
  python3 scripts/setup.py --force      # re-run wizard even if configs exist
  python3 scripts/setup.py --yes        # non-interactive (defaults + auto-install)
  python3 scripts/setup.py --device-id esp-abc123 --service-account ./sa.json

Idempotent: re-running never overwrites existing configs unless --force (then wizard re-asks).
Arduino libs/cores are now auto-installed by default (was --install).
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ── Config templates ────────────────────────────────────────────────────────
FRONTEND_EXAMPLE = ROOT / "frontend" / "config.example.js"
FRONTEND_TARGET = ROOT / "frontend" / "config.js"

FIREBASE_EXAMPLE = ROOT / "src" / "network" / "firebase_config.example.h"
FIREBASE_TARGET = ROOT / "src" / "network" / "firebase_config.h"

# Arduino libs as named in Library Manager (from README.md + verified via `arduino-cli lib search`)
ARDUINO_LIBS = [
    "Adafruit NeoPixel",
    "ESP Async WebServer",
    "AsyncTCP",
    "Firebase Arduino Client Library for ESP8266 and ESP32",
]

# FQBN + compile flags (from .workflow/VERIFICATION.log)
FQBN = "esp32:esp32:esp32s3"
COMPILE_EXTRA = [
    "FlashSize=4M",
    "PartitionScheme=no_fs",
    "CDCOnBoot=cdc",
    "PSRAM=enabled",
]

# ── helpers ─────────────────────────────────────────────────────────────────
def color(s, c):
    codes = {"red": 31, "green": 32, "yellow": 33, "cyan": 36, "dim": 2}
    if not sys.stdout.isatty():
        return s
    return f"\x1b[{codes[c]}m{s}\x1b[0m"

def ok(msg):  print(color("  ✔ ", "green") + msg)
def warn(msg): print(color("  ! ", "yellow") + msg)
def fail(msg): print(color("  ✘ ", "red") + msg)
def info(msg): print(color("  → ", "cyan") + msg)
def dim(msg):  print(color("    " + msg, "dim"))

def run(cmd, **kw):
    """Run cmd, return (returncode, stdout+stderr)."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, **kw)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except FileNotFoundError:
        return 127, f"not found: {cmd[0]}"

def have(cmd):
    return shutil.which(cmd) is not None

def ask(prompt, default="", allow_empty=True):
    """Prompt with [default]. Returns user input or default. Handles --yes / non-tty."""
    # called only when interactive; caller gates with is_interactive
    suffix = f" [{default}]" if default else ""
    try:
        val = input(f"    {prompt}{suffix}: ").strip()
    except EOFError:
        return default
    if not val:
        return default
    return val

def is_interactive(yes_flag):
    return not yes_flag

def ask_private_key(default="PASTE_YOUR_PRIVATE_KEY_HERE"):
    """Read private_key with multi-line support (paste -----BEGIN ... -----END)."""
    first = ask("private_key", default)
    # if user kept default or single-line escaped, return as-is
    if first == default:
        return first
    # if BEGIN without END, read continuation lines until END
    if "BEGIN PRIVATE KEY" in first and "END PRIVATE KEY" not in first:
        lines = [first]
        # next lines are raw pasted lines (no prompt prefix needed, but we show hint)
        dim("  (reading pasted key — keep pasting until -----END PRIVATE KEY-----)")
        while True:
            try:
                nxt = input().rstrip("\n")
            except EOFError:
                break
            # allow empty lines inside key
            lines.append(nxt)
            if "END PRIVATE KEY" in nxt:
                break
            # safety: stop if too many lines
            if len(lines) > 40:
                break
        pk = "\n".join(lines)
        return pk
    # handle single-line escaped \n → real newlines
    if "\\n" in first and "\n" not in first:
        # only convert if it looks like a key (contains BEGIN)
        if "BEGIN" in first:
            return first.replace("\\n", "\n")
    return first

def parse_frontend_config(path: Path):
    """Extract current values from existing config.js if any."""
    defaults = {
        "apiKey": "AIzaSyA1BCYnxBc9q_ONa58TTkGimlGPn0wyvj0",
        "authDomain": "esp32-electricity-counter.firebaseapp.com",
        "databaseURL": "https://esp32-electricity-counter-default-rtdb.firebaseio.com",
        "deviceId": "esp-000000",
        "adminEmails": "heng.xiao.hour@gmail.com",
    }
    if not path.exists():
        return defaults
    try:
        txt = path.read_text()
        import re
        # isolate the actual window.FB_CONFIG block to avoid matching comments
        m_block = re.search(r'window\.FB_CONFIG\s*=\s*\{([^}]*)\}', txt, re.S)
        block = m_block.group(1) if m_block else txt
        for k in ["apiKey", "authDomain", "databaseURL", "deviceId"]:
            m = re.search(rf'{k}\s*:\s*"([^"]*)"', block)
            if m:
                defaults[k] = m.group(1)
        m = re.search(r'adminEmails\s*:\s*\[(.*?)\]', block, re.S)
        if m:
            emails = re.findall(r'"([^"]+)"', m.group(1))
            if emails:
                defaults["adminEmails"] = ", ".join(emails)
    except Exception:
        pass
    return defaults

def write_frontend_config(values, dest: Path):
    content = f"""// ESP32 Counter — Firebase web config (generated by scripts/setup.py)
// Fill via: python3 scripts/setup.py --force
window.FB_CONFIG = {{
  apiKey: "{values['apiKey']}",
  authDomain: "{values['authDomain']}",
  databaseURL: "{values['databaseURL']}",
  deviceId: "{values['deviceId']}",
  adminEmails: [{", ".join(f'"{e.strip()}"' for e in values['adminEmails'].split(',') if e.strip())}]
}};
"""
    dest.write_text(content)
    ok(f"wrote {dest.relative_to(ROOT)}")

def parse_service_account_json(json_path: Path):
    """Return dict with project_id, client_email, private_key or None."""
    try:
        j = json.loads(json_path.read_text())
        return {
            "project_id": j.get("project_id", ""),
            "client_email": j.get("client_email", ""),
            "private_key": j.get("private_key", ""),
        }
    except Exception as e:
        warn(f"failed to parse {json_path}: {e}")
        return None

def write_firebase_config(values, dest: Path):
    # values: project_id, db_url, client_email, private_key
    pk = values.get("private_key", "")
    # ensure escaped for C string literal
    pk_escaped = pk.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n\" \\\n  \"")
    content = f"""#pragma once

// Firebase config — generated by scripts/setup.py (gitignored)
// Service-account auth (OAuth2). Keep FIREBASE_API_KEY empty.

#define FIREBASE_API_KEY ""        // NOT needed for service-account auth
#define FIREBASE_DB_URL "{values.get('db_url', '')}"
#define FIREBASE_PROJECT_ID "{values.get('project_id', 'esp32-electricity-counter')}"
#define FIREBASE_CLIENT_EMAIL "{values.get('client_email', '')}"
#define FIREBASE_PRIVATE_KEY \\
  "{pk_escaped}"
"""
    dest.write_text(content)
    ok(f"wrote {dest.relative_to(ROOT)}")

# ── Steps ───────────────────────────────────────────────────────────────────
def ensure_frontend_config(force=False, dry=False, yes=False, cli_overrides=None):
    print(f"\n{color('[1/5] Frontend config', 'cyan')}  frontend/config.js  — step-by-step wizard")
    if FRONTEND_TARGET.exists() and not force:
        ok(f"exists: {FRONTEND_TARGET.relative_to(ROOT)} (kept)")
        try:
            txt = FRONTEND_TARGET.read_text()
            if "window.FB_CONFIG" not in txt:
                warn("file exists but missing window.FB_CONFIG — check manually")
            else:
                dim("contains window.FB_CONFIG")
            if 'deviceId: "esp-000000"' in txt or "esp-000000" in txt:
                warn('deviceId still "esp-000000" — set to your board id (see src/utils/device_id.cpp)')
                if not dry and is_interactive(yes):
                    ans = ask("Re-configure frontend now? (y/N)", "n")
                    if ans.lower().startswith("y"):
                        force = True
                    else:
                        return True
                else:
                    dim("re-run with --force to re-configure step-by-step")
        except Exception as e:
            warn(f"read error: {e}")
        if not force:
            return True

    if dry:
        info(f"would run step-by-step wizard to create {FRONTEND_TARGET.name}")
        info("fields: apiKey, authDomain, databaseURL, deviceId, adminEmails")
        return True

    # gather defaults (existing file if force, else example)
    defaults = parse_frontend_config(FRONTEND_TARGET if FRONTEND_TARGET.exists() else FRONTEND_EXAMPLE)
    # cli overrides (e.g. --device-id)
    if cli_overrides and cli_overrides.get("deviceId"):
        defaults["deviceId"] = cli_overrides["deviceId"]

    interactive = is_interactive(yes)

    if interactive:
        print(color("    Fill frontend Firebase web config (press Enter to keep [default]):", "dim"))
        dim("Find values: Firebase Console → Project Settings → Your apps → Web app")
        values = {}
        values["apiKey"] = ask("Firebase apiKey (Web API key)", defaults["apiKey"])
        values["authDomain"] = ask("authDomain (<project>.firebaseapp.com)", defaults["authDomain"])
        values["databaseURL"] = ask("databaseURL (https://<project>-default-rtdb.firebaseio.com)", defaults["databaseURL"])
        values["deviceId"] = ask("deviceId (board chip ID, e.g. esp-1a2b3c; see src/utils/device_id.cpp)", defaults["deviceId"])
        values["adminEmails"] = ask("adminEmails (comma-separated Gmail, UI + RTDB rules)", defaults["adminEmails"])
    else:
        # non-interactive: use defaults / cli overrides, copy example verbatim if no overrides
        if cli_overrides and cli_overrides.get("deviceId"):
            defaults["deviceId"] = cli_overrides["deviceId"]
            write_frontend_config(defaults, FRONTEND_TARGET)
            if defaults["deviceId"] == "esp-000000":
                warn('deviceId is still "esp-000000" — re-run with --device-id or --force to set')
            return True
        # --yes without overrides: just copy example (preserves original behavior)
        if FRONTEND_EXAMPLE.exists():
            shutil.copy2(FRONTEND_EXAMPLE, FRONTEND_TARGET)
            ok(f"created {FRONTEND_TARGET.relative_to(ROOT)} from example (non-interactive)")
            warn('deviceId is "esp-000000" — re-run with --force or --device-id to set')
            return True
        values = defaults
        write_frontend_config(values, FRONTEND_TARGET)
        return True

    write_frontend_config(values, FRONTEND_TARGET)
    if values["deviceId"] == "esp-000000":
        warn('deviceId still "esp-000000" — set to your board unique ID for Cloud mode')
    return True

def ensure_firebase_config(force=False, dry=False, yes=False):
    print(f"\n{color('[2/5] Firmware Firebase config', 'cyan')}  src/network/firebase_config.h  — step-by-step wizard")
    if FIREBASE_TARGET.exists() and not force:
        ok(f"exists: {FIREBASE_TARGET.relative_to(ROOT)} (kept)")
        txt = FIREBASE_TARGET.read_text()
        if "PASTE_YOUR_PRIVATE_KEY_HERE" in txt or 'FIREBASE_CLIENT_EMAIL ""' in txt:
            warn("still contains placeholder — fill with service-account JSON")
            if not dry and is_interactive(yes):
                ans = ask("Re-configure firmware Firebase now? (y/N)", "n")
                if ans.lower().startswith("y"):
                    force = True
                else:
                    dim("Firebase Console → Project Settings → Service Accounts → Generate new private key")
                    return True
            else:
                dim("re-run with --force to fill step-by-step")
        else:
            dim("looks filled")
        jsons = list((ROOT / "src" / "network").glob("*firebase-adminsdk*.json"))
        if jsons:
            ok(f"service-account JSON found: {jsons[0].name}")
        else:
            warn("no *firebase-adminsdk*.json in src/network/ — needed for tools/hosting_deploy.py")
            dim("Save the downloaded JSON as src/network/<project>-firebase-adminsdk-*.json")
        if not force:
            return True

    if dry:
        info(f"would run step-by-step wizard to create {FIREBASE_TARGET.name}")
        info("fields: project_id, db_url, client_email, private_key  (or import service-account JSON)")
        return True

    interactive = is_interactive(yes)

    # optional JSON import
    sa_values = None
    sa_json_dest = None
    if interactive:
        print(color("    Firmware uses service-account JWT (not web apiKey).", "dim"))
        dim("Firebase Console → Project Settings → Service Accounts → Generate new private key → downloads JSON")
        json_hint = ""
        # check existing JSONs to suggest
        existing_jsons = list((ROOT / "src" / "network").glob("*firebase-adminsdk*.json"))
        if existing_jsons:
            json_hint = f" (found {existing_jsons[0].name})"
        path_in = ask(f"Path to service-account JSON (Enter to fill manually){json_hint}", "")
        if path_in:
            p = Path(path_in).expanduser()
            if not p.is_absolute():
                # try relative to ROOT
                alt = ROOT / path_in
                if alt.exists():
                    p = alt
            if p.exists():
                sa_values = parse_service_account_json(p)
                if sa_values:
                    ok(f"parsed JSON: project_id={sa_values['project_id']}, client_email={sa_values['client_email']}")
                    # copy JSON into src/network if not already there
                    dest = ROOT / "src" / "network" / p.name
                    if p.resolve() != dest.resolve():
                        try:
                            shutil.copy2(p, dest)
                            ok(f"copied service-account JSON → {dest.relative_to(ROOT)}")
                            sa_json_dest = dest
                        except Exception as e:
                            warn(f"copy failed: {e}")
                    else:
                        sa_json_dest = dest
                else:
                    warn("JSON parse failed — falling back to manual entry")
            else:
                warn(f"file not found: {p} — falling back to manual entry")

    # gather values
    if sa_values and sa_values.get("project_id"):
        # auto-fill from JSON, still allow step-by-step override
        project_id = sa_values.get("project_id", "esp32-electricity-counter")
        client_email = sa_values.get("client_email", "")
        private_key = sa_values.get("private_key", "")
        # derive db_url from project_id if not known
        default_db_url = f"https://{project_id}-default-rtdb.firebaseio.com"
        if interactive:
            print(color("    Confirm / override imported values (Enter to keep):", "dim"))
            project_id = ask("project_id", project_id)
            db_url = ask("database URL (FIREBASE_DB_URL)", default_db_url)
            client_email = ask("client_email (service account email)", client_email)
            # private_key is multi-line; show truncated
            pk_preview = private_key[:40].replace("\n", "\\n") + "..." if len(private_key) > 40 else private_key.replace("\n", "\\n")
            dim(f"current private_key preview: {pk_preview}")
            dim("To override key, paste new -----BEGIN ... -----END (multi-line supported) or Enter to keep")
            pk_in = ask_private_key("")
            if pk_in and "BEGIN PRIVATE KEY" in pk_in:
                private_key = pk_in
                if "\\n" in private_key and "\n" not in private_key:
                    private_key = private_key.replace("\\n", "\n")
            # else keep imported
        else:
            db_url = default_db_url
        values = {"project_id": project_id, "db_url": db_url, "client_email": client_email, "private_key": private_key}
        write_firebase_config(values, FIREBASE_TARGET)
        return True

    # manual step-by-step (or non-interactive defaults)
    if interactive:
        print(color("    Fill firmware Firebase values step-by-step (Enter to keep default):", "dim"))
        project_id = ask("project_id", "esp32-electricity-counter")
        db_url = ask("FIREBASE_DB_URL (https://<project>-default-rtdb.firebaseio.com)", f"https://{project_id}-default-rtdb.firebaseio.com")
        client_email = ask("FIREBASE_CLIENT_EMAIL (service account email)", "")
        # private key: read multi-line (paste) with helper
        print(color("    Paste private_key (single line with \\n or multi-line paste):", "dim"))
        dim("Example: -----BEGIN PRIVATE KEY-----\\nMIIE...\\n-----END PRIVATE KEY-----")
        dim("For multi-line paste, paste all lines including BEGIN/END — wizard reads until END")
        private_key = ask_private_key("PASTE_YOUR_PRIVATE_KEY_HERE")
        if "PASTE_YOUR" in private_key:
            warn("private_key still placeholder — Firebase will not connect until filled")
        values = {"project_id": project_id, "db_url": db_url, "client_email": client_email, "private_key": private_key}
        write_firebase_config(values, FIREBASE_TARGET)
        if not private_key or "PASTE_YOUR" in private_key or not client_email:
            warn("FIREBASE_CLIENT_EMAIL / PRIVATE_KEY incomplete — device Cloud mode will fail")
            dim("Re-run with --force after you have the service-account JSON")
        # remind about JSON file for hosting_deploy.py
        jsons = list((ROOT / "src" / "network").glob("*firebase-adminsdk*.json"))
        if not jsons:
            warn("no *firebase-adminsdk*.json in src/network/ — needed for tools/hosting_deploy.py")
            dim("Save the downloaded JSON as src/network/<project>-firebase-adminsdk-*.json")
        return True
    else:
        # non-interactive manual mode: just copy example
        if FIREBASE_EXAMPLE.exists():
            shutil.copy2(FIREBASE_EXAMPLE, FIREBASE_TARGET)
            ok(f"created {FIREBASE_TARGET.relative_to(ROOT)} from example (non-interactive)")
            warn("FIREBASE_CLIENT_EMAIL / PRIVATE_KEY still empty — re-run with --force (interactive) or paste JSON")
            return True
        return False

def check_python_deps(dry=False):
    print(f"\n{color('[3/5] Python dependencies', 'cyan')}")
    # cryptography is required for tools/firebase_rest.py token minting
    try:
        import cryptography  # noqa: F401
        ok(f"cryptography {__import__('cryptography').__version__} installed")
    except ImportError:
        fail("cryptography not installed — needed for tools/hosting_deploy.py")
        if dry:
            info("would run: pip install cryptography")
        else:
            print(color("    run: pip install cryptography", "yellow"))
        return False
    # optional: firebase CLI check handled separately
    return True

def check_arduino(install=True, dry=False, yes=False):
    print(f"\n{color('[4/5] Arduino toolchain', 'cyan')}  arduino-cli + ESP32 core + libs (auto-install missing)")
    if not have("arduino-cli"):
        fail("arduino-cli not found in PATH")
        dim("Install: https://arduino.github.io/arduino-cli/latest/installation/")
        return False
    rc, out = run(["arduino-cli", "version"])
    if rc == 0:
        ok(out.strip().splitlines()[0] if out else "arduino-cli found")
    else:
        warn("arduino-cli version check failed")

    # core
    rc, out = run(["arduino-cli", "core", "list"])
    if "esp32:esp32" in out:
        # parse version
        for line in out.splitlines():
            if "esp32:esp32" in line:
                ok(f"ESP32 core installed: {line.strip()}")
                break
    else:
        fail("ESP32 core esp32:esp32 not installed")
        if dry:
            info("would run: arduino-cli core install esp32:esp32")
            return False
        if install:
            info("installing esp32:esp32 core (may take a minute)...")
            rc2, out2 = run(["arduino-cli", "core", "install", "esp32:esp32"], timeout=300)
            if rc2 == 0:
                ok("ESP32 core installed")
            else:
                fail(f"core install failed: {out2[:400]}")
                return False
        else:
            # interactive prompt fallback (should not happen now, install=True default)
            if not dry and is_interactive(yes):
                ans = ask("Install ESP32 core now? (Y/n)", "y")
                if ans.lower().startswith("y"):
                    info("installing esp32:esp32 core...")
                    rc2, out2 = run(["arduino-cli", "core", "install", "esp32:esp32"], timeout=300)
                    if rc2 == 0:
                        ok("ESP32 core installed")
                    else:
                        fail(f"core install failed: {out2[:400]}")
                        return False
                else:
                    return False
            else:
                print(color("    run: arduino-cli core install esp32:esp32", "yellow"))
                return False

    # libs
    rc, out = run(["arduino-cli", "lib", "list"])
    installed = out if rc == 0 else ""
    all_ok = True
    missing = []
    for lib in ARDUINO_LIBS:
        if lib.lower() in installed.lower():
            ok(f'lib "{lib}" installed')
        else:
            alt = "Firebase Arduino Client" in lib and "firebase" in installed.lower()
            if alt:
                ok(f'lib "{lib}" installed (alias match)')
            else:
                fail(f'lib "{lib}" missing')
                missing.append(lib)
                all_ok = False

    if missing and not dry:
        # auto-install by default; interactive prompt if install=False
        should_install = install
        if not install:
            if is_interactive(yes):
                ans = ask(f"Install {len(missing)} missing Arduino lib(s) now? (Y/n)", "y")
                should_install = ans.lower().startswith("y")
            else:
                should_install = True  # non-interactive --yes: auto-install
        if should_install:
            for lib in missing:
                info(f'installing "{lib}"...')
                rc2, out2 = run(["arduino-cli", "lib", "install", lib], timeout=300)
                if rc2 == 0:
                    ok(f'lib "{lib}" installed')
                else:
                    # Firebase lib sometimes needs --git-url fallback; show error
                    fail(f'install failed for "{lib}": {out2[:500]}')
                    # try to give hint for Firebase
                    if "Firebase" in lib:
                        dim("Hint: Firebase Arduino Client may need: arduino-cli lib install 'Firebase Arduino Client Library for ESP8266 and ESP32'")
                    all_ok = False
            # re-check
            rc, out = run(["arduino-cli", "lib", "list"])
            installed2 = out if rc == 0 else ""
            all_ok = all(lib.lower() in installed2.lower() or ("Firebase Arduino Client" in lib and "firebase" in installed2.lower()) for lib in ARDUINO_LIBS)
            if all_ok:
                ok("all Arduino libs now installed")
        else:
            for lib in missing:
                print(color(f'    run: arduino-cli lib install "{lib}"', "yellow"))
            all_ok = False
    elif missing and dry:
        for lib in missing:
            info(f'would run: arduino-cli lib install "{lib}"')
        all_ok = False

    return all_ok

def check_firebase():
    print(f"\n{color('[5/5] Firebase deploy', 'cyan')}")
    if have("firebase"):
        rc, out = run(["firebase", "--version"])
        ok(f"firebase CLI {out.strip() if out else 'found'}")
        dim("deploy via: firebase deploy  OR  python3 scripts/deploy.py")
    else:
        warn("firebase CLI not found — optional")
        dim("REST deploy still works: python3 tools/hosting_deploy.py (needs service-account JSON)")
        dim("Install CLI: npm i -g firebase-tools  &&  firebase login")
    # hosting_deploy.py check
    hd = ROOT / "tools" / "hosting_deploy.py"
    if hd.exists():
        ok(f"tools/hosting_deploy.py present (CLI-free deploy)")
    # .firebaserc
    fb = ROOT / ".firebaserc"
    if fb.exists():
        try:
            j = json.loads(fb.read_text())
            proj = j.get("projects", {}).get("default", "?")
            ok(f".firebaserc project: {proj}")
        except Exception:
            warn(".firebaserc parse error")
    return True

def compile_firmware():
    print(f"\n{color('[compile] Firmware', 'cyan')}  arduino-cli compile")
    if not have("arduino-cli"):
        fail("arduino-cli not found — skip compile")
        return False
    fqbn = f"{FQBN}:" + ",".join(COMPILE_EXTRA)
    cmd = ["arduino-cli", "compile", "--fqbn", fqbn, "--warnings", "all", str(ROOT)]
    info(f"running: {' '.join(cmd)}")
    # stream output
    try:
        r = subprocess.run(cmd, cwd=ROOT)
        if r.returncode == 0:
            ok("compile PASS")
            return True
        else:
            fail(f"compile FAILED (exit {r.returncode})")
            return False
    except Exception as e:
        fail(str(e))
        return False

def main():
    ap = argparse.ArgumentParser(description="Setup esp32-electricity-counter (configs + deps + compile)")
    ap.add_argument("--check", action="store_true", help="only check, don't write files (no auto-install)")
    ap.add_argument("--install", action="store_true", help="install missing Arduino libs/cores (now default when not --check)")
    ap.add_argument("--no-install", action="store_true", help="skip Arduino lib install even if missing")
    ap.add_argument("--compile", action="store_true", help="also compile firmware (implies --install)")
    ap.add_argument("--force", action="store_true", help="re-run wizard even if configs exist")
    ap.add_argument("--yes", action="store_true", help="non-interactive (assume defaults, auto-install)")
    ap.add_argument("--device-id", help="set frontend deviceId non-interactively (e.g. esp-abc123)")
    ap.add_argument("--service-account", help="path to service-account JSON to auto-fill firmware config")
    args = ap.parse_args()

    if args.compile:
        args.install = True

    dry = args.check
    # auto-install by default unless --check or --no-install; --install is now implicit
    auto_install = not dry and not args.no_install
    if args.install:
        auto_install = True  # explicit flag keeps compat
    if dry:
        auto_install = False

    print(color("=== ESP32 Electricity Counter — Setup ===", "cyan"))
    print(f"  Project: {ROOT}")
    if dry:
        print(color("  Mode: --check (dry-run, no writes)", "yellow"))
    elif args.force:
        print(color("  Mode: --force (re-run wizard)", "yellow"))
    if args.yes:
        print(color("  Mode: --yes (non-interactive)", "yellow"))

    # handle --service-account non-interactive import for firmware
    service_import_ok = False
    if args.service_account and not dry:
        p = Path(args.service_account).expanduser()
        if not p.is_absolute():
            alt = ROOT / args.service_account
            if alt.exists():
                p = alt
        if p.exists():
            sa = parse_service_account_json(p)
            if sa:
                dest = ROOT / "src" / "network" / p.name
                try:
                    if p.resolve() != dest.resolve():
                        shutil.copy2(p, dest)
                        ok(f"copied service-account JSON → {dest.relative_to(ROOT)}")
                    values = {
                        "project_id": sa.get("project_id", "esp32-electricity-counter"),
                        "db_url": f"https://{sa.get('project_id','esp32-electricity-counter')}-default-rtdb.firebaseio.com",
                        "client_email": sa.get("client_email",""),
                        "private_key": sa.get("private_key",""),
                    }
                    write_firebase_config(values, FIREBASE_TARGET)
                    print(color("  → firmware config auto-filled from --service-account", "cyan"))
                    service_import_ok = True
                except Exception as e:
                    warn(f"service-account import failed: {e}")
            else:
                warn(f"failed to parse --service-account {p}")
        else:
            warn(f"--service-account file not found: {p}")

    cli_overrides = {}
    if args.device_id:
        cli_overrides["deviceId"] = args.device_id

    results = []
    # if service-account import succeeded, skip wizard (prevents --yes/--force overwriting)
    fw_already = service_import_ok
    results.append(("frontend/config.js", ensure_frontend_config(force=args.force, dry=dry, yes=args.yes, cli_overrides=cli_overrides)))
    if fw_already:
        print(f"\n{color('[2/5] Firmware Firebase config', 'cyan')}  src/network/firebase_config.h  — already filled via --service-account (skip)")
        results.append(("firebase_config.h", True))
    else:
        results.append(("firebase_config.h", ensure_firebase_config(force=args.force, dry=dry, yes=args.yes)))
    results.append(("python deps", check_python_deps(dry=dry)))
    results.append(("arduino", check_arduino(install=auto_install, dry=dry, yes=args.yes)))
    results.append(("firebase", check_firebase()))

    if args.compile and not dry:
        results.append(("compile", compile_firmware()))

    # summary
    print("\n" + color("=== Summary ===", "cyan"))
    all_pass = True
    for name, passed in results:
        mark = color("PASS", "green") if passed else color("FAIL", "red")
        # arduino may be partial — treat missing libs as not fatal unless --install
        print(f"  {mark}  {name}")
        if not passed:
            all_pass = False

    if dry:
        print("\n" + color("Dry-run done. Re-run without --check to create missing configs.", "yellow"))
        if not all_pass:
            print(color("Some checks failed — see above. Use --install to auto-fix Arduino libs.", "yellow"))
    else:
        print("\nNext steps:")
        if not FRONTEND_TARGET.exists():
            print("  1. frontend/config.js was not created — check errors above")
        elif 'esp-000000' in FRONTEND_TARGET.read_text():
            print("  1. Edit frontend/config.js → set deviceId to your board id (src/utils/device_id.cpp)")
        if not FIREBASE_TARGET.exists() or "PASTE_YOUR" in FIREBASE_TARGET.read_text():
            print("  2. Fill src/network/firebase_config.h with service-account values")
            print("     + save JSON as src/network/*firebase-adminsdk*.json for hosting_deploy.py")
        print("  3. Firebase deploy:  python3 tools/hosting_deploy.py   (no CLI)  OR  firebase deploy")
        print("  4. Arduino IDE: Board ESP32S3 Dev Module, Partition No FS 4MB (2MB APP with OTA), PSRAM Enabled")
        print("     CLI compile: arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=no_fs,CDCOnBoot=cdc ...")
        if args.compile:
            pass
        else:
            print("     Tip: re-run with --compile to verify firmware builds")

    sys.exit(0 if all_pass else 1)

if __name__ == "__main__":
    main()

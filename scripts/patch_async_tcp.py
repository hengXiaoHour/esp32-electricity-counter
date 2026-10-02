#!/usr/bin/env python3
"""
scripts/patch_async_tcp.py — apply the AsyncTCP 1.1.4 patches this project needs.

AsyncTCP 1.1.4 is incompatible with Arduino-ESP32 3.x in three ways. All three
patches are LOST every time the library is reinstalled or upgraded
(`arduino-cli lib install/upgrade AsyncTCP`, or the Arduino IDE Library Manager),
so run this again afterwards. It is idempotent: re-running is a no-op.

  PATCH 1 — const status()          (compile-time failure)
      ESPAsyncWebServer.h:1699 calls AsyncServer::status() through a const
      reference, so it must be declared const. Without this:
          error: passing 'const AsyncServer' as 'this' argument discards qualifiers
      Files: src/AsyncTCP.h  (declaration)
             src/AsyncTCP.cpp (definition)

  PATCH 2 — _tcp_new() wrapper      (runtime reboot loop)
      tcp_new_ip_type() reaches tcp_alloc(), which calls
      LWIP_ASSERT_CORE_LOCKED(). Arduino-ESP32 3.x ships
      CONFIG_LWIP_TCPIP_CORE_LOCKING=y with CONFIG_LWIP_CHECK_THREAD_SAFETY, and
      lwip/port/include/lwipopts.h asserts unless the caller IS the core-lock
      holder (the TCPIP thread). AsyncServer::begin() and AsyncClient::connect()
      called it raw from whatever task the caller was on, so starting the web
      server from a plain FreeRTOS task aborts the board:
          assert failed: tcp_alloc ... (Required to lock TCPIP core functionality!)
      Every other lwIP call in the file already goes through tcpip_api_call()
      (_tcp_bind, _tcp_listen_with_backlog, _tcp_connect, ...); PCB allocation
      was the only gap. This adds a matching _tcp_new() and routes both sites
      through it.  File: src/AsyncTCP.cpp

Usage:
    python3 scripts/patch_async_tcp.py            # patch (default)
    python3 scripts/patch_async_tcp.py --check    # report only, change nothing
    python3 scripts/patch_async_tcp.py --lib-dir PATH
    python3 scripts/patch_async_tcp.py --revert   # undo both patches

Exit codes: 0 ok / already patched · 1 error · 2 not found
"""
import argparse
import os
import shutil
import sys
from pathlib import Path

# --------------------------------------------------------------------------- #
# Locating the library
# --------------------------------------------------------------------------- #

def arduino_data_dirs():
    """Directories arduino-cli / the IDE use for user-installed libraries."""
    dirs = []
    for env in ("ARDUINO_DATA_DIR", "ARDUINO_SKETCHBOOK_DIR"):
        v = os.environ.get(env)
        if v:
            dirs.append(Path(v) / "libraries")
    dirs.append(Path.home() / "Arduino" / "libraries")
    dirs.append(Path.home() / ".arduino15" / "libraries")
    # Flatpak Arduino IDE
    dirs.append(Path.home() / ".var" / "app" / "org.arduino.ArduinoIDE"
                / "config" / "arduino15" / "libraries")
    dirs.append(Path.home() / "snap" / "arduino" / "current" / "Arduino" / "libraries")
    # Dedup, keep order
    seen, out = set(), []
    for d in dirs:
        if d not in seen:
            seen.add(d)
            out.append(d)
    return out


def find_lib_dir(explicit=None):
    """Return the AsyncTCP src/ directory, or None."""
    if explicit:
        p = Path(explicit).expanduser()
        candidates = [p / "src" / "AsyncTCP.cpp", p / "AsyncTCP.cpp"]
        for c in candidates:
            if c.is_file():
                return c.parent
        return None
    for base in arduino_data_dirs():
        if not base.is_dir():
            continue
        for entry in sorted(base.iterdir()):
            cpp = entry / "src" / "AsyncTCP.cpp"
            if cpp.is_file() and "asynctcp" in entry.name.lower():
                return cpp.parent
    return None


# --------------------------------------------------------------------------- #
# Patch definitions
# --------------------------------------------------------------------------- #
# Each patch is (name, [(old, new, required_anchor_or_None), ...]).
# A substitution whose `old` is absent and whose `new` is already present counts
# as "already applied" (idempotent). If neither is present it is a hard error,
# which means the library layout changed and the patch needs re-evaluating.

NEW_TCP_NEW_BLOCK = """// PATCH (project): marshal PCB allocation onto the TCPIP thread.
// tcp_new_ip_type() -> tcp_alloc() performs LWIP_ASSERT_CORE_LOCKED(), which
// (see lwip/port/include/lwipopts.h) only passes for the core-lock holder, i.e.
// the TCPIP thread. Arduino-ESP32 3.x ships CONFIG_LWIP_TCPIP_CORE_LOCKING=y
// with CONFIG_LWIP_CHECK_THREAD_SAFETY, so calling it directly from an ordinary
// FreeRTOS task aborts the board with:
//   assert failed: tcp_alloc ... (Required to lock TCPIP core functionality!)
// Every other lwIP call in this file is already wrapped this way (_tcp_bind,
// _tcp_listen_with_backlog, _tcp_connect, ...); PCB allocation was the gap.
static err_t _tcp_new_api(struct tcpip_api_call_data *api_call_msg){
    tcp_api_call_t * msg = (tcp_api_call_t *)api_call_msg;
    msg->err = 0;
    msg->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    return msg->err;
}

static tcp_pcb * _tcp_new(void) {
    tcp_api_call_t msg;
    msg.pcb = NULL;
    msg.closed_slot = -1;
    tcpip_api_call(_tcp_new_api, (struct tcpip_api_call_data*)&msg);
    return msg.pcb;
}

static err_t _tcp_bind_api(struct tcpip_api_call_data *api_call_msg){"""

TCP_NEW_MARKER = "static err_t _tcp_new_api("

PATCHES = [
    ("1-const-status-header", "AsyncTCP.h", [
        ("    uint8_t status();",
         "    uint8_t status() const;",
         "    uint8_t status() const;"),
    ]),
    ("1-const-status-cpp", "AsyncTCP.cpp", [
        ("uint8_t AsyncServer::status(){",
         "uint8_t AsyncServer::status() const{",
         "uint8_t AsyncServer::status() const{"),
    ]),
    ("2-tcp-new-wrapper", "AsyncTCP.cpp", [
        ("static err_t _tcp_bind_api(struct tcpip_api_call_data *api_call_msg){",
         NEW_TCP_NEW_BLOCK,
         TCP_NEW_MARKER),
    ]),
    ("2-tcp-new-callserver", "AsyncTCP.cpp", [
        ("    _pcb = tcp_new_ip_type(IPADDR_TYPE_V4);",
         "    _pcb = _tcp_new();   // PATCH: was tcp_new_ip_type() — needs TCPIP thread",
         "    _pcb = _tcp_new();   // PATCH"),
    ]),
    ("2-tcp-new-callclient", "AsyncTCP.cpp", [
        ("    tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_V4);",
         "    tcp_pcb* pcb = _tcp_new();   // PATCH: was tcp_new_ip_type() — needs TCPIP thread",
         "    tcp_pcb* pcb = _tcp_new();   // PATCH"),
    ]),
]

REVERTS = [
    ("AsyncTCP.h", "    uint8_t status() const;", "    uint8_t status();"),
    ("AsyncTCP.cpp", "uint8_t AsyncServer::status() const{",
     "uint8_t AsyncServer::status(){"),
    ("AsyncTCP.cpp", "    _pcb = _tcp_new();   // PATCH: was tcp_new_ip_type() — needs TCPIP thread",
     "    _pcb = tcp_new_ip_type(IPADDR_TYPE_V4);"),
    ("AsyncTCP.cpp", "    tcp_pcb* pcb = _tcp_new();   // PATCH: was tcp_new_ip_type() — needs TCPIP thread",
     "    tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_V4);"),
    ("AsyncTCP.cpp", NEW_TCP_NEW_BLOCK,
     "static err_t _tcp_bind_api(struct tcpip_api_call_data *api_call_msg){"),
]


def _codes(c):
    return {"red": 31, "green": 32, "yellow": 33, "cyan": 36, "dim": 2}


def col(s, c):
    if not sys.stdout.isatty():
        return s
    return f"\x1b[{_codes()[c]}m{s}\x1b[0m"


def ok(m):  print(col("  ✔ ", "green") + m)
def warn(m): print(col("  ! ", "yellow") + m)
def fail(m): print(col("  ✘ ", "red") + m)
def info(m): print(col("  → ", "cyan") + m)
def dim(m): print("    " + col(m, "dim"))


def backup(path: Path) -> None:
    bak = path.with_suffix(path.suffix + ".orig")
    if not bak.exists():
        shutil.copy2(path, bak)
        dim(f"backup: {bak.name}")


def apply_patches(src: Path, check_only: bool) -> int:
    """Returns number of problems (0 = success).

    In check_only mode, a patch that is NOT yet applied counts as a problem.
    That is the whole point of the mode: "--check" asks "is this library in the
    required state?", so "no" has to be a non-zero exit. It previously returned
    0 for both a fully patched and a fully unpatched library, which made it
    useless as a build gate - and a missing patch #2 (the tcp_new_ip_type
    tcpip_api_call wrapper) does not fail the build at all. It ships firmware
    that compiles cleanly and then reboot-loops at runtime on
    server->begin() with LWIP_ASSERT_CORE_LOCKED.
    """
    problems = 0
    by_file = {}
    for name, fname, subs in PATCHES:
        by_file.setdefault(fname, []).append((name, subs))

    for fname, entries in by_file.items():
        path = src / fname
        if not path.is_file():
            fail(f"{fname}: not found under {src}")
            problems += 1
            continue
        text = original = path.read_text()

        for name, subs in entries:
            applied_already = all(new in text for _, new, _ in subs)
            if applied_already:
                ok(f"{name}: already applied")
                continue
            changed = True
            for old, new, _anchor in subs:
                if new in text:
                    continue  # this substitution already done
                if old not in text:
                    fail(f"{name}: anchor not found — library layout changed, "
                         f"patch needs re-evaluating")
                    dim(f"expected: {old.strip()[:70]}")
                    problems += 1
                    changed = False
                    break
                text = text.replace(old, new, 1)
            if changed and text != original:
                info(f"{name}: patching")
                if not check_only:
                    backup(path)
                    path.write_text(text)
                    original = text
                else:
                    dim("(check mode — not written)")
                    # The library is NOT in the required state. In check mode
                    # that is a failure, not an informational note.
                    problems += 1
            elif not changed:
                continue
    return problems


def revert(src: Path) -> int:
    problems = 0
    for fname, new, old in REVERTS:
        path = src / fname
        if not path.is_file():
            continue
        text = path.read_text()
        if new in text:
            path.write_text(text.replace(new, old, 1))
            ok(f"reverted in {fname}")
        elif old in text:
            ok(f"{fname}: already pristine")
    return problems


def verify(src: Path) -> int:
    """Post-condition check. Returns number of failed expectations."""
    bad = 0
    h = (src / "AsyncTCP.h").read_text() if (src / "AsyncTCP.h").is_file() else ""
    c = (src / "AsyncTCP.cpp").read_text() if (src / "AsyncTCP.cpp").is_file() else ""

    def expect(cond, label):
        nonlocal bad
        if cond:
            ok(label)
        else:
            fail(label)
            bad += 1

    expect("uint8_t status() const;" in h, "patch 1: status() is const (header)")
    expect("uint8_t AsyncServer::status() const{" in c, "patch 1: status() is const (definition)")
    expect(TCP_NEW_MARKER in c, "patch 2: _tcp_new_api() wrapper present")
    expect("_pcb = _tcp_new();" in c, "patch 2: AsyncServer::begin() uses _tcp_new()")
    expect("tcp_pcb* pcb = _tcp_new();" in c, "patch 2: AsyncClient::connect() uses _tcp_new()")

    # No *unguarded* allocation left. Comments legitimately mention
    # tcp_new_ip_type() (the PATCH notes and the wrapper's own body), so strip
    # comments before looking for a real call.
    def strip_comments(src: str) -> str:
        out, i, n = [], 0, len(src)
        while i < n:
            if src.startswith("//", i):
                j = src.find("\n", i)
                i = n if j < 0 else j
            elif src.startswith("/*", i):
                j = src.find("*/", i + 2)
                i = n if j < 0 else j + 2
            else:
                out.append(src[i])
                i += 1
        return "".join(out)

    code = strip_comments(c)
    expect(code.count("tcp_new_ip_type(") == 1,
           "patch 2: exactly one tcp_new_ip_type() call remains (inside _tcp_new_api)")
    return bad


def main():
    ap = argparse.ArgumentParser(description="Patch AsyncTCP 1.1.4 for Arduino-ESP32 3.x")
    ap.add_argument("--check", action="store_true",
                    help="report status only, change nothing")
    ap.add_argument("--lib-dir", help="explicit path to the AsyncTCP library")
    ap.add_argument("--revert", action="store_true", help="undo both patches")
    ap.add_argument("--no-verify", action="store_true", help="skip post-condition check")
    args = ap.parse_args()

    print(col("=== AsyncTCP patch (Arduino-ESP32 3.x compatibility) ===", "cyan"))

    src = find_lib_dir(args.lib_dir)
    if src is None:
        fail("AsyncTCP library not found")
        if args.lib_dir:
            dim(f"checked: {args.lib_dir}")
        else:
            for d in arduino_data_dirs():
                dim(f"searched: {d}")
            dim("pass --lib-dir PATH, or install the library first:")
            dim('  arduino-cli lib install "AsyncTCP"')
        return 2

    info(f"library: {src}")

    if args.revert:
        revert(src)
    else:
        problems = apply_patches(src, check_only=args.check)
        if problems:
            print()
            fail(f"{problems} problem(s) — see above")
            if args.check:
                print()
                fail("AsyncTCP is NOT in the required state. Either patches are")
                fail("missing (a library install/upgrade wiped them) or an anchor")
                fail("no longer matches. Build with:")
                fail("    python3 scripts/patch_async_tcp.py")
            return 1
        if not args.check and not args.no_verify:
            print()
            info("verifying")
            verify(src)

    print()
    if args.check:
        dim("check mode — nothing was written")
    return 0


if __name__ == "__main__":
    sys.exit(main())

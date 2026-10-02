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

  PATCH 3 — callback registration   (runtime reboot loop)
      Same root cause, different API. Registering the PCB callbacks is just as
      core-locked as allocating it: tcp_arg, tcp_recv, tcp_sent, tcp_err,
      tcp_poll and tcp_accept all run LWIP_ASSERT_CORE_LOCKED(). Patch 2 fixed
      only the allocation, so the very next line aborted the board instead:
          assert failed: tcp_arg /IDF/components/lwip/lwip/src/core/tcp.c:2035
              (Required to lock TCPIP core functionality!)
      which is the reboot loop reported on 2026-10-02: the app boots, prints the
      AP banner, dies on wsServer.startServer() -> AsyncServer::begin(), and
      reboots, forever. Adds _tcp_set_data_callbacks() / _tcp_set_listen_callbacks()
      and routes the five call sites that run on an APPLICATION task through them.

      Deliberately NOT patched, because they already execute on the TCPIP thread
      and tcpip_api_call() there would deadlock:
          AsyncClient::AsyncClient(tcp_pcb*)   (only caller is _accept)
          AsyncClient::_error() / _lwip_fin()  (lwIP callbacks)
      File: src/AsyncTCP.cpp

Usage:
    python3 scripts/patch_async_tcp.py            # patch (default)
    python3 scripts/patch_async_tcp.py --check    # report only, change nothing
    python3 scripts/patch_async_tcp.py --lib-dir PATH
    python3 scripts/patch_async_tcp.py --revert   # undo both patches

Exit codes: 0 ok / already patched · 1 error · 2 not found
"""
import argparse
import os
import re
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
CB_MARKER = "static err_t _tcp_set_callbacks_api("

# PATCH 3 -------------------------------------------------------------------
# Registering callbacks is core-locked exactly like allocating the PCB, and
# patch 2 proved it the hard way: after fixing tcp_new_ip_type(), the board got
# one line further and then aborted on tcp_arg() instead.
#
# Both helpers run on the TCPIP thread and are NOT safe to call from a lwIP
# callback (that would deadlock on tcpip_api_call). Each site patched below is
# reached from an application task; the ones left alone already run there.
SET_CALLBACKS_BLOCK = """// PATCH (project): callback registration is core-locked too.
// tcp_arg/tcp_recv/tcp_sent/tcp_err/tcp_poll (and tcp_accept on a listening PCB)
// all run LWIP_ASSERT_CORE_LOCKED(), so calling them straight from an
// application task aborts the board:
//   assert failed: tcp_arg /IDF/components/lwip/lwip/src/core/tcp.c:2035
//          (Required to lock TCPIP core functionality!)
// They all mutate the same PCB, so the whole group goes through ONE api call to
// keep it atomic with respect to the TCPIP thread.
// DO NOT call these from a lwIP callback (AsyncServer::_accept,
// AsyncClient::_error, AsyncClient::_lwip_fin): those already run on the TCPIP
// thread and tcpip_api_call() would deadlock there.
typedef struct {
    struct tcpip_api_call_data call;
    tcp_pcb * pcb;
    void * arg;
    tcp_recv_fn recv_cb;
    tcp_sent_fn sent_cb;
    tcp_err_fn err_cb;
    tcp_poll_fn poll_cb;
    tcp_accept_fn accept_cb;
} tcp_callbacks_api_t;

static err_t _tcp_set_callbacks_api(struct tcpip_api_call_data *api_call_msg){
    tcp_callbacks_api_t * msg = (tcp_callbacks_api_t *)api_call_msg;
    msg->err = ERR_OK;
    tcp_arg(msg->pcb, msg->arg);
    if (msg->recv_cb)   { tcp_recv(msg->pcb, msg->recv_cb); }
    if (msg->sent_cb)   { tcp_sent(msg->pcb, msg->sent_cb); }
    if (msg->err_cb)    { tcp_err(msg->pcb, msg->err_cb); }
    if (msg->poll_cb)   { tcp_poll(msg->pcb, msg->poll_cb, 1); }
    if (msg->accept_cb) { tcp_accept(msg->pcb, msg->accept_cb); }
    return msg->err;
}

static void _tcp_set_data_callbacks(tcp_pcb * pcb, void * arg,
        tcp_recv_fn recv_cb, tcp_sent_fn sent_cb,
        tcp_err_fn err_cb, tcp_poll_fn poll_cb){
    if (pcb == NULL) { return; }
    tcp_callbacks_api_t msg;
    msg.pcb = pcb;
    msg.arg = arg;
    msg.recv_cb = recv_cb;
    msg.sent_cb = sent_cb;
    msg.err_cb = err_cb;
    msg.poll_cb = poll_cb;
    msg.accept_cb = NULL;          // never touch tcp_accept on a data PCB
    tcpip_api_call(_tcp_set_callbacks_api, (struct tcpip_api_call_data*)&msg);
}

static void _tcp_set_listen_callbacks(tcp_pcb * pcb, void * arg,
        tcp_accept_fn accept_cb){
    if (pcb == NULL) { return; }
    tcp_callbacks_api_t msg;
    msg.pcb = pcb;
    msg.arg = arg;
    msg.recv_cb = NULL;            // a listening PCB has no data callbacks
    msg.sent_cb = NULL;
    msg.err_cb = NULL;
    msg.poll_cb = NULL;
    msg.accept_cb = accept_cb;
    tcpip_api_call(_tcp_set_callbacks_api, (struct tcpip_api_call_data*)&msg);
}

static err_t _tcp_bind_api(struct tcpip_api_call_data *api_call_msg){"""

# The four data-PCB sites, replaced one at a time. `old` must be unique in the
# file -- each is anchored on a neighbouring line that makes it so.
CB_SUBS = [
    # AsyncClient::operator=
    # The anchor reaches back over `_closed_slot = other._closed_slot;` on purpose:
    # the five-line tcp_arg/tcp_recv/tcp_sent/tcp_err/tcp_poll block appears TWICE
    # verbatim (the AsyncClient(pcb) constructor and operator=), so anchoring on the
    # block itself patched the CONSTRUCTOR — the one site that must stay raw, since
    # its only caller is AsyncServer::_accept on the TCPIP thread — and left the
    # application-task site unpatched.
    ("    _pcb = other._pcb;\n"
     "    _closed_slot = other._closed_slot;\n"
     "    if (_pcb) {\n"
     "        _rx_last_packet = millis();\n"
     "        tcp_arg(_pcb, this);\n"
     "        tcp_recv(_pcb, &_tcp_recv);\n"
     "        tcp_sent(_pcb, &_tcp_sent);\n"
     "        tcp_err(_pcb, &_tcp_error);\n"
     "        tcp_poll(_pcb, &_tcp_poll, 1);\n",
     "    _pcb = other._pcb;\n"
     "    _closed_slot = other._closed_slot;\n"
     "    if (_pcb) {\n"
     "        _rx_last_packet = millis();\n"
     "        _tcp_set_data_callbacks(_pcb, this, &_tcp_recv, &_tcp_sent, &_tcp_error, &_tcp_poll);   // PATCH: was 5 raw core-locked calls\n"),
    # AsyncClient::connect()
    ("    tcp_arg(pcb, this);\n"
     "    tcp_err(pcb, &_tcp_error);\n"
     "    tcp_recv(pcb, &_tcp_recv);\n"
     "    tcp_sent(pcb, &_tcp_sent);\n"
     "    tcp_poll(pcb, &_tcp_poll, 1);\n",
     "    _tcp_set_data_callbacks(pcb, this, &_tcp_recv, &_tcp_sent, &_tcp_error, &_tcp_poll);   // PATCH: was 5 raw core-locked calls\n"),
    # AsyncClient::_close()  (clearing them is equally core-locked)
    ("        tcp_arg(_pcb, NULL);\n"
     "        tcp_sent(_pcb, NULL);\n"
     "        tcp_recv(_pcb, NULL);\n"
     "        tcp_err(_pcb, NULL);\n"
     "        tcp_poll(_pcb, NULL, 0);\n"
     "        _tcp_clear_events(this);\n",
     "        _tcp_set_data_callbacks(_pcb, NULL, NULL, NULL, NULL, NULL);   // PATCH: was 5 raw core-locked calls\n"
     "        _tcp_clear_events(this);\n"),
    # AsyncServer::begin() -- the line that was aborting the board
    ("    tcp_arg(_pcb, (void*) this);\n"
     "    tcp_accept(_pcb, &_s_accept);\n",
     "    _tcp_set_listen_callbacks(_pcb, (void*) this, &_s_accept);   // PATCH: was 2 raw core-locked calls\n"),
    # AsyncServer::end()
    ("        tcp_arg(_pcb, NULL);\n"
     "        tcp_accept(_pcb, NULL);\n",
     "        _tcp_set_listen_callbacks(_pcb, NULL, NULL);   // PATCH: was 2 raw core-locked calls\n"),
]

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
    ("3-callback-helpers", "AsyncTCP.cpp", [
        ("static err_t _tcp_bind_api(struct tcpip_api_call_data *api_call_msg){",
         SET_CALLBACKS_BLOCK,
         CB_MARKER),
    ]),
    *[(f"3-call-{i}", "AsyncTCP.cpp", [(old, new, new)])
      for i, (old, new) in enumerate(CB_SUBS)],
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
    # Patch 3 is reverted before patch 2, because patch 3's anchor line is the
    # one patch 2 consumes; reverting in list order restores a pristine file.
    *[(("AsyncTCP.cpp", new, old)) for old, new in reversed(CB_SUBS)],
    ("AsyncTCP.cpp", SET_CALLBACKS_BLOCK,
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
            # "Already applied" must be decided by the ANCHOR (a short marker that
            # survives later patches), never by the full replacement text. The two
            # helper blocks both end at the same _tcp_bind_api line, so once the
            # callback block is inserted in front of it the tcp_new block's own
            # text is no longer contiguous — testing for that made a patched
            # library look unpatched and the script cheerfully inserted a SECOND
            # copy of both helpers on the next run.
            anchors = [_anchor for _, _, _anchor in subs if _anchor]
            applied_already = all(a in text for a in anchors) if anchors else all(
                new in text for _, new, _ in subs)
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
        else:
            # Neither the patched nor the pristine form is here: the file moved
            # under us. Staying quiet about that is how a half-reverted library
            # gets shipped.
            fail(f"{fname}: neither patched nor pristine text found — "
                 f"library layout changed, patch needs re-evaluating")
            dim(f"expected one of:")
            dim(f"  new: {new.strip().splitlines()[0][:70]}")
            dim(f"  old: {old.strip().splitlines()[0][:70]}")
            problems += 1
    return problems


def _function_bodies(src: str, names) -> dict:
    """Map each named function to its body, brace-matched.

    Written because a substring count cannot distinguish "calls the wrapper
    through tcpip_api_call" from "calls it directly", and the difference
    between those two is the entire bug.
    """
    import re
    out = {}
    for name in names:
        m = re.search(r"^static\s+\w[\w \*]*\b" + re.escape(name) + r"\s*\(", src, re.M)
        if not m:
            continue
        start = src.index("{", m.end())
        depth, i = 0, start
        while i < len(src):
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out[name] = src[start:i + 1]
    return out


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
    expect(CB_MARKER in c, "patch 3: _tcp_set_callbacks_api() wrapper present")
    expect("_tcp_set_data_callbacks(" in c and "_tcp_set_listen_callbacks(" in c,
           "patch 3: both helper wrappers present")

    # Patch 3 must rewrite EVERY application-task site, not just the one that
    # happened to abort. Assert on the exact call count, and assert that the
    # three already-TCPIP-thread blocks were deliberately left alone (marshalling
    # those would deadlock the board instead of fixing it).
    for i, (old, new) in enumerate(CB_SUBS):
        expect(c.count(old) == 0,
               f"patch 3: site {i} no longer calls lwIP raw "
               f"(still present {c.count(old)}x)")
        expect(c.count(new) == 1, f"patch 3: site {i} rewritten exactly once")
    expect(c.count("        tcp_arg(_pcb, this);\n") == 1,
           "patch 3: AsyncClient ctor left raw (already on the TCPIP thread)")
    expect("        tcp_arg(_pcb, NULL);\n        if(_pcb->state == LISTEN) {" in c,
           "patch 3: _error()/_lwip_fin() left raw (lwIP callbacks)")
    expect(c.count("static void _tcp_set_data_callbacks(") == 1
           and c.count("static void _tcp_set_listen_callbacks(") == 1,
           "patch 3: helpers defined exactly once")

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

    # The whole point of patch 3: no core-locked callback call may remain on any
    # path that runs on an application task. Every survivor must be either inside
    # the marshalling helper, or one of the three sites that already execute on
    # the TCPIP thread. Enumerated per line so a survivor can be NAMED, which
    # makes a wrong count diagnosable instead of just red.
    #
    # Counts below are derived from the file's own structure, not guessed:
    #   1 helper site            (inside _tcp_set_callbacks_api, on the TCPIP thread)
    # + 1 AsyncClient(pcb) ctor  (only caller is AsyncServer::_accept)
    # + 2 lwIP callbacks         (_error, _lwip_fin)
    EXPECTED = {"tcp_arg": 4, "tcp_recv": 4, "tcp_sent": 4,
                "tcp_err": 4, "tcp_poll": 4, "tcp_accept": 1}
    # The sites that MUST still be raw, quoted so the assertion below fails if a
    # future edit removes them (which would mean marshalling a TCPIP-thread path
    # and deadlocking the board instead of fixing it).
    MUST_STAY_RAW = [
        "tcp_arg(_pcb, this);",          # AsyncClient(pcb) ctor
        "tcp_recv(_pcb, &_tcp_recv);",
        "tcp_sent(_pcb, &_tcp_sent);",
        "tcp_err(_pcb, &_tcp_error);",
        "tcp_poll(_pcb, &_tcp_poll, 1);",
    ]
    for frag in MUST_STAY_RAW:
        expect(code.count(frag) >= 1,
               f"patch 3: TCPIP-thread site left raw as intended ({frag})")

    for fn, want in EXPECTED.items():
        # A real call is not preceded by '_' (which is how the log_e() strings and
        # the _tcp_* wrappers spell the same name).
        n = len(re.findall(r"(?<![_A-Za-z])" + fn + r"\s*\(", code))
        expect(n == want, f"patch 3: {want} {fn}() call sites expected, got {n}")
    # The helpers must actually marshal, BOTH of them, and nothing may call the
    # _tcp_set_callbacks_api() wrapper directly instead — a direct call runs the
    # lwIP work on the CALLER's thread, which is the exact bug this patch exists
    # to remove, and a count on the name alone could not tell the two apart.
    helpers = _function_bodies(c, ("_tcp_set_data_callbacks", "_tcp_set_listen_callbacks"))
    expect(len(helpers) == 2,
           f"patch 3: both helper definitions found, got {len(helpers)}")
    for fname, body in helpers:
        expect("tcpip_api_call(_tcp_set_callbacks_api," in body,
               f"patch 3: {fname}() marshals via tcpip_api_call()")
        expect("_tcp_set_callbacks_api((struct" not in body,
               f"patch 3: {fname}() does not call the wrapper directly")
    expect(code.count("tcpip_api_call(_tcp_set_callbacks_api,") == 2,
           "patch 3: exactly two tcpip_api_call sites, one per helper")
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
        # --check runs the post-condition verifier too. Without this, --check only
        # asked "are the anchors findable", so a library whose helpers had been
        # edited to stop marshalling still passed --check cleanly. A check mode
        # is only a gate if it asserts the state the build actually depends on.
        if not args.no_verify:
            print()
            info("verifying" + (" (check mode — reporting only)" if args.check else ""))
            if verify(src) and args.check:
                return 1

    print()
    if args.check:
        dim("check mode — nothing was written")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Mock ESP32-S3 counter — serves frontend/ and speaks the real /ws protocol.

Why this exists: the device serves its own dashboard, so the frontend's most
important integration point (same-origin http -> ws://<host>/ws) can only be
verified by running the real page against something that behaves like the board.
Without it, every frontend claim in this change would rest on "the JSON looks
right".

It mirrors the firmware deliberately:
  * static routes match WebSocketServer::findAsset() - exact match only,
    "/" -> /index.html, query string stripped, no directory walking;
  * the /ws snapshot uses the same field names buildSystemJson() emits;
  * the admin gate is a line-for-line port of src/network/auth_gate.cpp, NOT a
    paraphrase. If the two ever disagree, this harness stops being evidence -
    the PIN tests in scripts/test_auth_gate.c are the authority on that logic.

Not a full firmware emulator: sensing, NVS, auto-zero and OTA are faked.

Usage:  python3 scripts/mock_device.py --port 8099
        python3 scripts/mock_device.py --port 8099 --pin 1234
"""

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Overridable so the frontend can be pointed at a MUTATED COPY. Mutation
# testing is the only way to know an E2E suite is not a tautology: a suite that
# has only ever printed PASS has proved nothing.
FRONTEND = os.environ.get("MOCK_FRONTEND", os.path.join(ROOT, "frontend"))

# Same table as scripts/embed_web.py. config.js is gone: it held the Firebase
# web config and the board no longer talks to Firebase.
ASSETS = {
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
    "/script.js": ("script.js", "application/javascript; charset=utf-8"),
    "/manifest.json": ("manifest.json", "application/manifest+json"),
    "/sw.js": ("sw.js", "application/javascript; charset=utf-8"),
    "/icons/icon-192.png": ("icons/icon-192.png", "image/png"),
    "/icons/icon-512.png": ("icons/icon-512.png", "image/png"),
    "/icons/apple-touch-icon.png": ("icons/apple-touch-icon.png", "image/png"),
    "/icons/maskable-192.png": ("icons/maskable-192.png", "image/png"),
    "/icons/maskable-512.png": ("icons/maskable-512.png", "image/png"),
}

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PIN_EXEMPT_VERBS = ("set_time", "verify_pin")

DEFAULTS = {
    "voltage_cal": 260.0,
    # Sized by NUM_CHANNELS in src/config.h (5); check_docs.py compares these
    # lengths with the header so a channel-count change fails the gate instead
    # of silently desyncing the mock from the firmware.
    "current_cal": [100.0] * 5,
    "noise_floor": [0.0] * 5,
    "lpf_alpha": [0.2] * 5,
    "rms_samples": 1000,
}

# Factory identity, matching src/config.h. Kept as literals here on purpose: the
# mock must not import the firmware's headers, and a drift between this and
# config.h is caught by scripts/check_docs.py.
AP_SSID_DEFAULT = "ESP32-Elec-Counter"
AP_PASS_DEFAULT = "configure123"


def json_number(frame, key):
    """Value of a numeric JSON field, e.g. json_number(f, "ch") -> 0.

    Stops at the first character that is not part of the number. Reading the
    rest of the frame and keeping every digit is wrong: in
    {"cmd":"set_name","ch":0,"pin":"1234"} that yields 01234.
    """
    i = frame.find('"%s":' % key)
    if i < 0:
        return None
    i += len(key) + 3
    num = ""
    for ch in frame[i:]:
        if ch.isdigit() or ch in "-.":
            num += ch
        else:
            break
    if not num or num in ("-", "."):
        return None
    try:
        return float(num) if "." in num else int(num)
    except ValueError:
        return None


def extract_verb(frame):
    """Port of auth_gate.cpp extractVerb()."""
    i = frame.find('"cmd":"')
    if i < 0:
        return None
    i += 7
    j = frame.find('"', i)
    if j < 0 or j == i:
        return None
    return frame[i:j]


def extract_pin(frame):
    """Port of auth_gate.cpp auth_extractPin(), including the truncated-frame
    rule: an unterminated field is malformed and must NOT authenticate."""
    needle = '"pin":"'
    i = frame.find(needle)
    if i < 0:
        return None
    i += len(needle)
    out = []
    while i < len(frame):
        c = frame[i]
        if c == "\\" and i + 1 < len(frame):
            nxt = frame[i + 1]
            i += 2
            out.append({"n": "\n", "t": "\t", "r": "\r"}.get(nxt, nxt))
            if len(out) >= 63:
                return None
            continue
        if c == '"':
            return "".join(out)
        out.append(c)
        i += 1
        if len(out) >= 63:
            return None
    return None  # truncated -> reject


def auth_check(frame, expected_pin):
    """Port of auth_gate.cpp auth_check(). Returns (allowed, verb)."""
    verb = extract_verb(frame)
    if verb is None:
        return True, None            # not the gate's business
    if verb in PIN_EXEMPT_VERBS:
        return True, verb
    got = extract_pin(frame)
    if not got:
        return False, verb
    if not expected_pin:
        return False, verb
    return got == expected_pin, verb


# --- AP credential rules -------------------------------------------------
# Port of src/network/ap_creds.cpp, including the exact reason strings: the
# E2E suite asserts on them, so a mock that paraphrased the firmware would let
# the frontend drift away from the device without anything failing. Lengths are
# OCTETS (what strlen counts), not characters.
AP_MAX_SSID_LEN = 32
AP_MIN_PASS_LEN = 8
AP_MAX_PASS_LEN = 63


def _has_control(s):
    return any(ord(c) < 0x20 for c in s)


def ap_validate_ssid(ssid):
    """Returns a reason string, or None when the SSID is usable."""
    if ssid is None:
        return "Network name is missing"
    if len(ssid.encode("utf-8")) == 0:
        return "Network name cannot be empty"
    if len(ssid.encode("utf-8")) > AP_MAX_SSID_LEN:
        return "Network name must be 32 characters or fewer"
    if _has_control(ssid):
        return "Network name cannot contain control characters"
    if ssid.startswith(" ") or ssid.endswith(" "):
        return "Network name cannot start or end with a space"
    if ssid and all(c == " " for c in ssid):
        return "Network name cannot be only spaces"
    return None


def ap_validate_pass(pass_):
    if pass_ is None:
        return "Password is missing"
    n = len(pass_.encode("utf-8"))
    if n == 0:
        return "Password cannot be empty (an open network is not allowed)"
    if n < AP_MIN_PASS_LEN:
        return "Password must be at least 8 characters"
    if n > AP_MAX_PASS_LEN:
        return "Password must be 63 characters or fewer"
    if _has_control(pass_):
        return "Password cannot contain control characters"
    return None


def ap_validate(ssid, pass_):
    return ap_validate_ssid(ssid) or ap_validate_pass(pass_)


def extract_json_string(frame, key):
    """Value of a JSON string field, honouring backslash escapes.

    Mirrors command_processor.cpp's extractJsonString() closely enough for the
    fields the mock handles. Returns None when the field is absent or
    unterminated - an unterminated field is malformed, not empty.
    """
    needle = '"%s":"' % key
    i = frame.find(needle)
    if i < 0:
        return None
    i += len(needle)
    out = []
    while i < len(frame):
        c = frame[i]
        if c == "\\" and i + 1 < len(frame):
            nxt = frame[i + 1]
            i += 2
            out.append({"n": "\n", "t": "\t", "r": "\r"}.get(nxt, nxt))
            continue
        if c == '"':
            return "".join(out)
        out.append(c)
        i += 1
    return None      # truncated -> malformed


class State:
    """Everything the mock 'remembers' between frames."""

    def __init__(self, pin):
        self.lock = threading.Lock()
        self.pin = pin
        self.names = ["Living Room AC", "Kitchen", "Office", "Server Rack",
                      "Washing Machine"]
        self.limits = [48.0, 30.0, 25.0, 60.0, 20.0]
        self.kwh = [12.4, 8.1, 4.9, 31.2, 6.7]
        self.station_up = False        # set by `setwifi`, cleared by `clearwifi`
        self.sta_ssid = ""             # the home network this board joins
        self.sta_pass = ""
        self.epoch = int(time.time())
        self.time_synced = True
        self.time_rejects = 0
        self.applied = []             # log of accepted mutations, for assertions
        # The board's own network identity, in "flash". The mock cannot reboot,
        # so it records the pending reboot instead of performing one - the E2E
        # suite asserts on `rebooted`, which is what the deferred restart in
        # ConsoleHandler::cmdReboot would cause.
        self.ap_ssid = AP_SSID_DEFAULT
        self.ap_pass = AP_PASS_DEFAULT
        self.rebooted = False
        self.reboot_reason = None

    def snapshot(self):
        with self.lock:
            ch = []
            # len(self.names), not a literal: NUM_CHANNELS in src/config.h.
            for i in range(len(self.names)):
                over = self.limits[i] > 0 and self.kwh[i] >= self.limits[i]
                ch.append({
                    "n": self.names[i],
                    "a": round(1.2 + 0.35 * i, 2),
                    "w": round(45.0 + 30.0 * i, 1),
                    "va": round(60.0 + 35.0 * i, 1),
                    "pf": 0.87,
                    "kwh": self.kwh[i],
                    "s": 2 if over else 0,
                    "mkwh": self.limits[i],
                })
            return {
                "v": 231.4,
                "uptime": 4242,
                "wifi": bool(self.station_up),  # STA link to the home network
                "rssi": -58 if self.station_up else 0,
                # STA-first: the fallback AP is up exactly when STA is down.
                "ap": not self.station_up,
                "voltageCalibration": DEFAULTS["voltage_cal"],
                "currentCalibration": list(DEFAULTS["current_cal"]),
                "rmsSamples": DEFAULTS["rms_samples"],
                "noiseFloor": list(DEFAULTS["noise_floor"]),
                "azActive": False,
                "azChannel": -1,
                "azProgress": 0,
                "azQueue": [],
                "lpfAlpha": list(DEFAULTS["lpf_alpha"]),
                "firmwareVersion": "3.0.0",
                "epoch": self.epoch,
                "lastMonth": 202610,
                "apSsid": self.ap_ssid,
                "apIsDefault": (self.ap_ssid == AP_SSID_DEFAULT
                                and self.ap_pass == AP_PASS_DEFAULT),
                "staSsid": self.sta_ssid,
                "staIsDefault": not self.sta_ssid,
                "mcuTemp": 51.2,
                "eco": False,
                "time": {"ok": self.time_synced, "age": 0 if self.time_synced else 4294967295},
                "ch": ch,
                "events": [
                    {"t": self.epoch - 90, "c": 0, "s": 2, "v": 49.0,
                     "m": "Monthly limit reached — over budget"},
                ],
            }

    def apply(self, frame, respond):
        """Returns (handled, response_text, auth_rejected)."""
        with self.lock:
            verb = extract_verb(frame)
            if not verb:
                return False, None, False

            allowed, _ = auth_check(frame, self.pin)
            if not allowed:
                return False, "  Admin PIN required (Settings -> Admin PIN)", True

            if verb == "verify_pin":
                # Must actually compare, exactly like the firmware does. This
                # was a stub returning "PIN OK" unconditionally, which made a
                # CORRECT board look like it accepted any PIN - the mock
                # disagreed with the code it stands in for.
                given = extract_pin(frame)
                ok = bool(given) and given == self.pin
                return True, ("  PIN OK" if ok else "  PIN incorrect"), False

            if verb == "set_pin":
                # set_pin carries the CURRENT pin in "pin" (already validated by
                # the gate above) and the new one in "pin_new".
                ni = frame.find('"pin_new":"')
                new = None
                if ni >= 0:
                    ni += len('"pin_new":"')
                    nj = frame.find('"', ni)
                    if nj > ni:
                        new = frame[ni:nj]
                if new and 4 <= len(new) <= 16:
                    self.pin = new
                    self.applied.append(("set_pin", new))
                    return True, "  Admin PIN changed", False
                return True, "  PIN must be 4-16 characters", False

            if verb == "set_time":
                epoch = json_number(frame, "t")
                if epoch is None:
                    return False, None, False
                if 1609459200 <= epoch <= 4102444800:
                    self.epoch = epoch
                    self.time_synced = True
                    self.applied.append(("set_time", epoch))
                    return True, "  Clock set to epoch %d" % epoch, False
                self.time_rejects += 1
                return False, None, False

            if verb == "set_name":
                ch = json_number(frame, "ch")
                ni = frame.find('"name":"')
                if ch is None or ni < 0:
                    return False, None, False
                nj = frame.find('"', ni + 8)
                if 0 <= ch < 6 and nj > ni + 8:
                    self.names[ch] = frame[ni + 8:nj]
                    self.applied.append(("set_name", ch, self.names[ch]))
                    return True, None, False

            if verb == "set_monthly_kwh":
                ch = json_number(frame, "ch")
                val = json_number(frame, "val")
                if ch is None or val is None:
                    return False, None, False
                if 0 <= ch < 6 and val > 0:
                    self.limits[ch] = float(val)
                    self.applied.append(("set_monthly_kwh", ch, float(val)))
                    return True, None, False

            if verb == "reset_counter":
                ch = json_number(frame, "ch")
                if ch is None or not (0 <= ch < 6):
                    return False, None, False
                self.kwh[ch] = 0.0
                self.applied.append(("reset_counter", ch))
                return True, None, False

            if verb == "console":
                return True, "  Unknown command. Type 'help'.", False

            if verb == "set_ap":
                ssid = extract_json_string(frame, "ssid")
                pass_ = extract_json_string(frame, "pass")
                if ssid is None or pass_ is None:
                    return True, "  Both a network name and a password are required", False
                # Same order as ap_creds_validate(): report the name first.
                reason = ap_validate(ssid, pass_)
                if reason:
                    # REJECTED: nothing stored. The firmware returns handled ==
                    # False here, so a client that only reloads on success gets
                    # the same treatment here.
                    return False, "  Not saved: " + reason, False
                self.ap_ssid = ssid
                self.ap_pass = pass_
                self.applied.append(("set_ap", ssid, pass_))
                self.rebooted = True
                self.reboot_reason = "set_ap"
                return True, ('  Saved. The board is restarting on network "%s"'
                              ' - join that WiFi and reopen the page.' % ssid), False

            if verb == "reset_ap":
                self.ap_ssid = AP_SSID_DEFAULT
                self.ap_pass = AP_PASS_DEFAULT
                self.applied.append(("reset_ap",))
                self.rebooted = True
                self.reboot_reason = "reset_ap"
                return True, ('  Reset to "%s" / "%s". The board is restarting.'
                              % (AP_SSID_DEFAULT, AP_PASS_DEFAULT)), False

            if verb == "setwifi":
                ssid = extract_json_string(frame, "ssid")
                pass_ = extract_json_string(frame, "pass") or ""
                if ssid is None or not ssid:
                    return True, "  Not saved: the SSID is empty.", False
                self.sta_ssid = ssid
                self.sta_pass = pass_
                self.station_up = True        # the reboot would come up joined
                self.applied.append(("setwifi", ssid, pass_))
                self.rebooted = True
                self.reboot_reason = "setwifi"
                return True, ('  Saved. The board will join "%s" on boot.'
                              % ssid), False

            if verb == "clearwifi":
                self.sta_ssid = ""
                self.sta_pass = ""
                self.station_up = False
                self.applied.append(("clearwifi",))
                return True, "  Stored WiFi credentials cleared", False

            return False, None, False


# ---------------------------------------------------------------- HTTP / WS


def send_ws_frame(conn, payload, opcode=0x1):
    data = payload.encode("utf-8") if isinstance(payload, str) else payload
    header = bytearray()
    header.append(0x80 | opcode)
    n = len(data)
    if n < 126:
        header.append(n)
    elif n < (1 << 16):
        header.append(126)
        header += struct.pack(">H", n)
    else:
        header.append(127)
        header += struct.pack(">Q", n)
    conn.sendall(bytes(header) + data)


def recv_ws_frame(conn):
    """Returns (opcode, payload_bytes) or None on close."""
    def readn(n):
        buf = b""
        while len(buf) < n:
            chunk = conn.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    hdr = readn(2)
    if not hdr:
        return None
    opcode = hdr[0] & 0x0F
    masked = bool(hdr[1] & 0x80)
    ln = hdr[1] & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", readn(2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", readn(8))[0]
    mask = readn(4) if masked else None
    data = readn(ln) if ln else b""
    if data is None:
        return None
    if mask:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return opcode, data


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    state = None
    clients = []

    def log_message(self, *a):
        pass

    # ---- static -------------------------------------------------------
    def do_GET(self):
        if "websocket" in self.headers.get("Upgrade", "").lower():
            self.handle_ws()
            return
        path = self.path.split("?")[0]
        if path in ("", "/"):
            path = "/index.html"
        entry = ASSETS.get(path)
        if not entry:
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", "9")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.end_headers()
            self.wfile.write(b"Not found")
            return
        rel, mime = entry
        with open(os.path.join(FRONTEND, rel), "rb") as fh:
            body = fh.read()
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.end_headers()
        self.wfile.write(body)

    # ---- websocket ----------------------------------------------------
    def handle_ws(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self.send_response(400)
            self.end_headers()
            return
        accept = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.wfile.flush()

        conn = self.connection
        stop = threading.Event()
        Handler.clients.append(conn)

        # Two threads write to this socket: the 150 ms snapshot broadcaster and
        # the read loop that answers commands. Without a lock, two concurrent
        # sendall() calls interleave and corrupt a frame header, and the client
        # silently loses whatever it was being told - which showed up as an E2E
        # assertion with an empty message log and no obvious cause. Real single-
        # threaded firmware does not have this problem; the harness did.
        send_lock = threading.Lock()

        def send(payload, opcode=0x1):
            with send_lock:
                send_ws_frame(conn, payload, opcode)

        # Broadcast loop, like the firmware's 150 ms cadence.
        def broadcaster():
            while not stop.is_set():
                try:
                    send(json.dumps(self.state.snapshot()))
                except OSError:
                    break
                stop.wait(0.15)

        th = threading.Thread(target=broadcaster, daemon=True)
        th.start()

        try:
            while not stop.is_set():
                frame = recv_ws_frame(conn)
                if frame is None:
                    break
                opcode, payload = frame
                if opcode == 0x8:      # close
                    break
                if opcode == 0x9:      # ping
                    send(payload, opcode=0xA)
                    continue
                if opcode != 0x1:
                    continue
                try:
                    text = payload.decode("utf-8")
                except UnicodeDecodeError:
                    continue
                handled, response, rejected = self.state.apply(text, None)
                if rejected:
                    send('{"type":"auth","ok":false}')
                elif response:
                    send(json.dumps({"type": "console", "out": response}))
                if handled:
                    send(json.dumps(self.state.snapshot()))
        except (OSError, ConnectionResetError):
            pass
        finally:
            stop.set()
            if conn in Handler.clients:
                Handler.clients.remove(conn)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--pin", default="1234")
    args = ap.parse_args()

    Handler.state = State(args.pin)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(8)
    print("mock board on http://127.0.0.1:%d/  (pin=%s)" % (args.port, args.pin),
          flush=True)
    while True:
        conn, _ = srv.accept()
        t = threading.Thread(target=_serve, args=(conn,), daemon=True)
        t.start()


def _serve(conn):
    # BaseHTTPRequestHandler wants to own the socket for keep-alive parsing.
    class S(Handler):
        pass
    S.state = Handler.state
    try:
        S(conn, ("127.0.0.1", 0), None)
    except (OSError, ConnectionResetError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


if __name__ == "__main__":
    main()
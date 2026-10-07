#!/usr/bin/env python3
"""Host test for scripts/setup.py: full wizard against a fake board on a pty.

No hardware needed. A FakeBoard thread speaks the exact reply lines the
firmware prints (console_handler.cpp), so the test proves the wizard sends
byte-exact commands, survives both reboots, and prints the dashboard IP.
"""
import io
import os
import pty
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
import setup

PASS = []
FAIL = []


def check(name, cond, detail=""):
    (PASS if cond else FAIL).append(name)
    print(("ok   " if cond else "FAIL ") + name +
          ("" if cond else " -- %s" % detail))


BANNER_AP = ("  WiFi                 AP FALLBACK (home network failed)\n"
             '  Network:           "ESP32-Elec-Counter"\n'
             "  Dashboard:         http://192.168.4.1/\n"
             "> ")
BANNER_STA = ("  WiFi                 STA (AP off)\n"
              "  Dashboard:         http://10.0.0.5/\n"
              "> ")
WIFI_UP = ("  Mode:            STA (AP off)\n"
           "  Home network:\n"
           "  SSID:            Home\n"
           "  Status:          CONNECTED\n"
           "  RSSI:            -60 dBm\n"
           "  IP:              10.0.0.5\n"
           "  Dashboard:       http://10.0.0.5/\n")


class FakeBoard(threading.Thread):
    """Answers console lines on the pty master; records what the wizard sent."""

    def __init__(self, master, refuse_wifi=False):
        super().__init__(daemon=True)
        self.master = master
        self.lines = []
        self.refuse_wifi = refuse_wifi
        self.buf = b""

    def send(self, text):
        os.write(self.master, text.encode())

    def run(self):
        self.send(BANNER_AP)
        while True:
            try:
                chunk = os.read(self.master, 4096)
            except OSError:
                return
            if not chunk:
                time.sleep(0.01)
                continue
            self.buf += chunk
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                self.answer(line.decode("utf-8", "replace").strip())

    def answer(self, line):
        self.lines.append(line)
        if line.startswith("setwifi "):
            if self.refuse_wifi:
                self.send("  Usage: setwifi <ssid> <password>\n> ")
            else:
                ssid = line[len("setwifi "):].split(" ", 1)[0].strip('"')
                self.send('  Saved. The board will join "%s" on boot.\n'
                          "  (station WiFi changed - restarting)\n" % ssid)
                time.sleep(0.05)
                self.send(BANNER_STA)
        elif line.startswith("setcloud "):
            email = line.split(" ")[2] if len(line.split(" ")) > 2 else "?"
            self.send('  Saved. Signing in as "%s" on boot (STA only).\n'
                      "  (cloud monitoring changed - restarting)\n" % email)
            time.sleep(0.05)
            self.send(BANNER_STA)
        elif line == "wifi":
            self.send(WIFI_UP + "> ")
        else:
            self.send("  Unknown command. Type help.\n> ")


def run_wizard(argv, fake_kwargs=None):
    """Run main() with a fake board; return (exit, stdout, fake)."""
    master, slave = pty.openpty()
    slave_path = os.ttyname(slave)
    fake = FakeBoard(master, **(fake_kwargs or {}))
    fake.start()
    # Fast waits: the fake answers instantly, nothing real needs settling.
    setup.SETTLE_S, setup.CMD_WAIT_S, setup.WIFI_WAIT_S = 0.05, 0.3, 0.3
    old, buf = sys.stdout, io.StringIO()
    sys.stdout = buf
    try:
        code = setup.main(["--port", slave_path] + argv)
    except SystemExit as e:
        code = e.code
    finally:
        sys.stdout = old
        setup.SETTLE_S, setup.CMD_WAIT_S, setup.WIFI_WAIT_S = 2.0, 6.0, 4.0
    out = buf.getvalue()
    # Let the fake thread die with the fds.
    try:
        os.close(slave)
    except OSError:
        pass
    try:
        os.close(master)
    except OSError:
        pass
    return code, out, fake


def main():
    # --- pure logic ---
    check("plain ssid is sent bare", setup.quote_arg("Home") == "Home")
    check("ssid with spaces is quoted",
          setup.quote_arg("My Router") == '"My Router"')
    try:
        setup.quote_arg('evil"ssid')
        check("embedded quote is refused", False)
    except ValueError:
        check("embedded quote is refused", True)
    try:
        setup.quote_arg("a\nb")
        check("newline is refused", False)
    except ValueError:
        check("newline is refused", True)
    check("setwifi quoting is exact",
          setup.build_setwifi("My Router", "pw") == 'setwifi "My Router" pw')
    check("setcloud order is host email pass",
          setup.build_setcloud("h", "e", "p") == "setcloud h e p")
    check("empty ssid rejected", setup.validate_ssid("") is not None)
    check("33-char ssid rejected", setup.validate_ssid("x" * 33) is not None)
    check("32-char ssid accepted", setup.validate_ssid("x" * 32) is None)
    check("empty wifi pass rejected",
          setup.validate_wifi_pass("") is not None)
    check("https host rejected",
          setup.validate_cloud("https://h.io", "e", "p") is not None)
    check("path host rejected",
          setup.validate_cloud("h.io/x", "e", "p") is not None)
    check("dotless host rejected",
          setup.validate_cloud("nodot", "e", "p") is not None)
    check("bad email rejected",
          setup.validate_cloud("h.io", "no-at", "p") is not None)
    check("good cloud accepted",
          setup.validate_cloud("h.io", "b@e.l", "p") is None)
    check("Saved marker passes",
          setup.reply_ok('  Saved. The board will join "H" on boot.', 'will join "'))
    check("usage text fails even with marker nearby",
          not setup.reply_ok("Usage: x will join \"", 'will join "'))
    check("refusal fails",
          not setup.reply_ok("  Not saved: the SSID is empty.", 'will join "'))
    check("banner ip parses",
          setup.parse_dashboard_ip(BANNER_STA) == "10.0.0.5")
    check("wifi block ip parses",
          setup.parse_dashboard_ip(WIFI_UP) == "10.0.0.5")
    check("no ip gives None", setup.parse_dashboard_ip("hello") is None)

    # --- reboot-wait retry logic (fake poll, no sleeping on success) ---
    calls = []
    setup.wait_for_port("x", lambda p: calls.append(p) or True, timeout=2)
    check("port wait returns at once when present", calls == ["x"])
    t0 = time.time()
    ok = setup.wait_for_port("x", lambda p: False, timeout=0.6)
    check("port wait times out when absent", ok is False and
          time.time() - t0 < 2.0)

    # --- full flows against the fake board ---
    code, out, fake = run_wizard(["--ssid", "Home", "--wifi-pass", "pw",
                                  "--no-cloud", "--yes"])
    check("wifi-only flow exits 0", code == 0, repr(code))
    check("exact setwifi bytes on the wire",
          "setwifi Home pw" in fake.lines, repr(fake.lines))
    check("wizard asks for status after reboot", "wifi" in fake.lines)
    check("summary prints the dashboard ip",
          "http://10.0.0.5/" in out, out[-200:])

    code, out, fake = run_wizard(["--ssid", "My Router", "--wifi-pass", "pw",
                                  "--no-cloud", "--yes"])
    check("spaced ssid is quoted on the wire",
          'setwifi "My Router" pw' in fake.lines, repr(fake.lines))

    code, out, fake = run_wizard(["--ssid", "Home", "--wifi-pass", "pw",
                                  "--cloud-host", "h.io",
                                  "--cloud-email", "b@e.l",
                                  "--cloud-pass", "s3cret", "--yes"])
    check("cloud flow exits 0", code == 0, repr(code))
    check("exact setcloud bytes on the wire",
          "setcloud h.io b@e.l s3cret" in fake.lines, repr(fake.lines))
    check("summary names the cloud account", "b@e.l" in out, out[-200:])

    code, out, fake = run_wizard(["--ssid", "RefuseMe", "--wifi-pass", "pw",
                                  "--no-cloud", "--yes"],
                                 fake_kwargs={"refuse_wifi": True})
    check("board refusal exits non-zero", code != 0, repr(code))
    check("refusal says what happened",
          isinstance(code, str) and "refused" in code, repr(code))

    code, out, fake = run_wizard(["--check"])
    check("check mode exits 0", code == 0, repr(code))
    check("check prints the ip", "10.0.0.5" in out, out[-200:])

    print("setup wizard: %d checks passed, %d failed"
          % (len(PASS), len(FAIL)))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())

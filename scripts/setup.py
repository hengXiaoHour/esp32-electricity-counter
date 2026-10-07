#!/usr/bin/env python3
"""First-run provisioning for esp32-electricity-counter over USB serial.

Walks the same steps README Option 1 describes, guided:

    python3 scripts/setup.py                # interactive: port, WiFi, cloud
    python3 scripts/setup.py --check        # only print WiFi/dashboard status
    python3 scripts/setup.py --port /dev/ttyACM0 --ssid "Home" --wifi-pass x \\
        --cloud-host h --cloud-email e --cloud-pass p --yes   # no prompts

Serial is PIN-less (physical access is the key), so no admin PIN is asked.
`setwifi` / `setcloud` each reboot the board; on native-USB chips the tty
vanishes briefly, so the port is closed and re-opened between steps - the
same reason scripts/serial_cmd.py keeps one long-lived fd per command.
"""
import argparse
import getpass
import glob
import os
import re
import sys
import time

try:
    import fcntl
    import termios
    import struct
except ImportError:  # exercised on POSIX hosts; guarded for import-time safety
    fcntl = termios = struct = None

BAUD = 115200
SETTLE_S = 2.0
CMD_WAIT_S = 6.0
REBOOT_WAIT_S = 20.0

# Buffer sizes mirror the firmware, so the wizard refuses exactly what the
# board would refuse (src/network/ap_creds.h, src/network/cloud_cfg.h).
MAX_SSID = 32
MAX_WIFI_PASS = 63
MAX_HOST = 128
MAX_EMAIL = 128
MAX_CLOUD_PASS = 128


# --- pure logic (no IO; covered by scripts/test_setup.py) -----------------

def quote_arg(s):
    """Quote for the board's console splitter: only when spaces are present.

    A double quote inside the value is refused (the firmware splitter has no
    escape for it), never silently mangled.
    """
    if '"' in s or "\n" in s or "\r" in s:
        raise ValueError("value must not contain a double quote or newline")
    if any(c in s for c in " \t"):
        return '"%s"' % s
    return s


def build_setwifi(ssid, password):
    return "setwifi %s %s" % (quote_arg(ssid), password)


def build_setcloud(host, email, password):
    return "setcloud %s %s %s" % (host, email, password)


def _nonempty(name, s, limit):
    if not s:
        return "%s is empty" % name
    if len(s) > limit:
        return "%s is %d chars (max %d)" % (name, len(s), limit)
    return None


def validate_ssid(ssid):
    return _nonempty("SSID", ssid, MAX_SSID)


def validate_wifi_pass(password):
    return _nonempty("WiFi password", password, MAX_WIFI_PASS)


def validate_cloud(host, email, password):
    for name, s, lim in (("database host", host, MAX_HOST),
                         ("account email", email, MAX_EMAIL),
                         ("account password", password, MAX_CLOUD_PASS)):
        err = _nonempty(name, s, lim)
        if err:
            return err
    if "://" in host or "/" in host:
        return "database host is host only (no https://, no path)"
    if "." not in host:
        return "database host looks wrong (no dot in it)"
    if "@" not in email:
        return "account email has no @ in it"
    return None


def reply_ok(reply, marker):
    """A mutating command worked when its Saved marker is present and the
    board did not answer with usage/refusal instead."""
    return marker in reply and "Usage:" not in reply and "Not saved" not in reply


def parse_dashboard_ip(text):
    """Pull the Dashboard URL the banner and `wifi` both print."""
    m = re.search(r"Dashboard:\s*http://([0-9.]+)/", text)
    return m.group(1) if m else None


# --- serial link (one long-lived fd, like scripts/serial_cmd.py) ----------

def open_fd(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(fd)
    iflag = oflag = lflag = 0
    cflag = termios.CLOCAL | termios.CREAD | termios.CS8
    termios.tcsetattr(fd, termios.TCSANOW,
                      [iflag, oflag, cflag, lflag, BAUD, BAUD, list(cc)])
    try:  # clear DTR/RTS: asserting them holds the chip in reset
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("i", 3))
    except OSError:
        pass
    return fd


def drain(fd, seconds):
    out = b""
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            chunk = os.read(fd, 4096)
        except (BlockingIOError, OSError):
            time.sleep(0.05)
            continue
        if chunk:
            out += chunk
    return out


class Board(object):
    """A console session. `opener` returns a fresh fd (re-called after every
    reboot, because native USB re-enumerates). Split out so tests can hand a
    pty instead of real hardware."""

    def __init__(self, opener, settle=SETTLE_S):
        self._opener = opener
        self._settle = settle
        self.fd = None

    def open(self):
        self.fd = self._opener()
        return drain(self.fd, self._settle).decode("utf-8", "replace")

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    def run(self, cmd, wait=CMD_WAIT_S):
        os.write(self.fd, (cmd + "\n").encode())
        return drain(self.fd, wait).decode("utf-8", "replace")


def wait_for_port(path, poll, timeout=REBOOT_WAIT_S):
    """After a rebooting command the tty may vanish (native USB). Wait for
    the same path to come back instead of talking into a dead fd."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if poll(path):
            return True
        time.sleep(0.5)
    return False


def find_ports():
    found = []
    for pat in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        found.extend(p for p in sorted(glob.glob(pat)) if os.path.exists(p))
    return found


# --- the wizard -----------------------------------------------------------

def ask(prompt, secret=False, default=None):
    if default:
        prompt = "%s [%s]: " % (prompt, default)
    else:
        prompt = "%s: " % prompt
    if secret:
        got = getpass.getpass(prompt)
    else:
        got = input(prompt)
    got = got.strip()
    return got or default or ""


def pick_port(args):
    if args.port:
        return args.port
    ports = find_ports()
    if len(ports) == 1:
        print("Using %s (only serial port found)." % ports[0])
        return ports[0]
    if not ports:
        sys.exit("No /dev/ttyACM* or /dev/ttyUSB* found. Plug the board in "
                 "or pass --port.")
    print("Serial ports:")
    for i, p in enumerate(ports):
        print("  %d) %s" % (i + 1, p))
    if args.yes:
        sys.exit("--yes needs an unambiguous port: pass --port.")
    while True:
        try:
            n = int(ask("Pick one", default="1"))
        except ValueError:
            continue
        if 1 <= n <= len(ports):
            return ports[n - 1]


def main(argv=None):
    ap = argparse.ArgumentParser(description="Provision the counter over USB.")
    ap.add_argument("--port")
    ap.add_argument("--ssid")
    ap.add_argument("--wifi-pass")
    ap.add_argument("--cloud-host")
    ap.add_argument("--cloud-email")
    ap.add_argument("--cloud-pass")
    ap.add_argument("--no-cloud", action="store_true",
                    help="skip the cloud mirror, WiFi only")
    ap.add_argument("--yes", action="store_true",
                    help="no prompts; all values must come from flags")
    ap.add_argument("--check", action="store_true",
                    help="only print WiFi/dashboard status")
    ap.add_argument("--baud", type=int, default=BAUD)
    args = ap.parse_args(argv)
    globals()["BAUD"] = args.baud

    port = pick_port(args)
    board = Board(lambda: open_fd(port))

    try:
        banner = board.open()
    except OSError as e:
        sys.exit("Cannot open %s: %s" % (port, e))

    if args.check:
        reply = board.run("wifi", wait=4.0)
        print(reply)
        ip = parse_dashboard_ip(banner + reply)
        print("Dashboard: http://%s/" % ip if ip else "Board has no IP yet.")
        return 0

    interactive = not args.yes and sys.stdin.isatty()
    ssid = args.ssid
    wifi_pass = args.wifi_pass
    if ssid is None or wifi_pass is None:
        if not interactive:
            sys.exit("--yes needs --ssid and --wifi-pass.")
        print("Home WiFi (the network the board joins on boot):")
        if ssid is None:
            ssid = ask("SSID")
        if wifi_pass is None:
            wifi_pass = ask("Password", secret=True)
    err = validate_ssid(ssid) or validate_wifi_pass(wifi_pass)
    if err:
        sys.exit("Not sent: %s." % err)

    try:
        reply = board.run(build_setwifi(ssid, wifi_pass))
    except ValueError as e:
        sys.exit("Not sent: %s." % e)
    if not reply_ok(reply, 'will join "'):
        print(reply)
        sys.exit("The board refused setwifi (see above).")
    print('WiFi saved. Board is rebooting onto "%s"...' % ssid)

    board.close()
    if not wait_for_port(port, os.path.exists):
        sys.exit("The port never came back after reboot. Unplug/replug, "
                 "then re-run with --port %s." % port)
    board.open()

    cloud = None
    if not args.no_cloud:
        host, email, cpass = args.cloud_host, args.cloud_email, args.cloud_pass
        if host is None or email is None or cpass is None:
            if not interactive:
                if host is None and email is None and cpass is None:
                    print("No cloud flags given: skipping the cloud mirror.")
                else:
                    sys.exit("--yes needs all of --cloud-host/--cloud-email/"
                             "--cloud-pass, or none of them (--no-cloud).")
            else:
                print("Cloud mirror, optional (Enter to skip):")
                if host is None:
                    host = ask("Database host (no https://)", default="")
                if host:
                    if email is None:
                        email = ask("Board account email", default="")
                    if cpass is None:
                        cpass = ask("Board account password", secret=True)
        if host:
            err = validate_cloud(host, email, cpass)
            if err:
                sys.exit("Not sent: %s." % err)
            reply = board.run(build_setcloud(host, email, cpass))
            if not reply_ok(reply, "Signing in as"):
                print(reply)
                sys.exit("The board refused setcloud (see above).")
            print('Cloud saved. Board is rebooting and signs in as "%s"...'
                  % email)
            cloud = email
            board.close()
            if not wait_for_port(port, os.path.exists):
                sys.exit("The port never came back after reboot. Unplug/"
                         "replug, then re-run with --port %s." % port)
            board.open()

    reply = board.run("wifi", wait=4.0)
    ip = parse_dashboard_ip(reply)
    print("")
    print("Done. Home WiFi: %s%s" % (ssid, " + cloud (%s)" % cloud if cloud
                                     else " (no cloud mirror)"))
    if ip:
        print("Dashboard: http://%s/  (same WiFi as the board)" % ip)
    else:
        print("Board has no IP yet (still joining?). Re-run with --check.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

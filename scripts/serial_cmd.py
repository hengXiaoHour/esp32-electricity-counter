"""Send a command to the board's serial console and print what it answers.

    python3 scripts/serial_cmd.py 'set_ap "Lab Meter" bench2026pass' [wait]

One long-lived fd on /dev/ttyACM0. Reopening the port in a loop resets the
ESP32-S3 (cdc_acm asserts DTR+RTS on open, which the devkit wires to EN/BOOT)
and makes the board look like it is rebooting several times a second.
"""
import os
import sys
import time
import fcntl
import termios
import struct

PORT = os.environ.get("SERIAL_PORT", "/dev/ttyACM0")
CMD = sys.argv[1] if len(sys.argv) > 1 else "help"
WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(fd)
    iflag = oflag = lflag = 0
    cflag = termios.CLOCAL | termios.CREAD | termios.CS8
    cc = list(cc)
    termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, 115200, 115200, cc])
    try:  # clear DTR/RTS: asserting them holds the chip in reset
        # Mask is DTR|RTS bits (0x2|0x4 = 6), NOT 3: mask 3 clears LE+DTR
        # and leaves RTS asserted, which halts a CP210x/CH340 board after a
        # 64-byte ROM banner (looks dead). Verified on /dev/ttyUSB0.
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack('I', 6))
    except OSError:
        pass

    # Let the banner settle, then send on the prompt.
    t0 = time.time()
    pre = b""
    while time.time() - t0 < 2.0:
        try:
            pre += os.read(fd, 4096)
        except (BlockingIOError, OSError):
            time.sleep(0.05)
    try:
        os.read(fd, 65536)
    except (BlockingIOError, OSError):
        pass

    os.write(fd, (CMD + "\n").encode())

    out = b""
    t0 = time.time()
    while time.time() - t0 < WAIT:
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            time.sleep(0.05)
            continue
        except OSError:
            break
        if chunk:
            out += chunk

    print("----- board said -----")
    print(out.decode("utf-8", "replace"))
finally:
    os.close(fd)
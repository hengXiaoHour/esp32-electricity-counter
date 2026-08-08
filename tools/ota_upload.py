#!/usr/bin/env python3
"""
ota_upload.py — flash an ESP32 anywhere in the world over Firebase.

Feels like the Arduino IDE "Upload" button, but the board can be on any
network on Earth: the firmware is delivered through Firebase (Hosting + RTDB).

    python3 tools/ota_upload.py 1.3.0
    python3 tools/ota_upload.py 1.3.0 --bin build/firmware.bin

Pipeline:
  1. compile the firmware            (or use --bin to skip compiling)
  2. compute md5
  3. publish the .bin to Firebase Hosting  -> https://<site>.web.app/firmware/<ver>.bin
  4. write RTDB /ota {version,url,md5,ts}
  5. poll /ota/status, render a progress bar until the device reboots
  6. wait for the device to boot and confirm /latest fw == <ver>

Why Hosting and not Storage: the project has no billing account, and creating a
GCS bucket requires one. Firebase Hosting serves the binary for free on the
Spark plan. The device still verifies the download with md5.
"""
import argparse
import hashlib
import re
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FQBN = "esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=no_fs,CDCOnBoot=cdc"
SCOPES = " ".join("https://www.googleapis.com/auth/" + s for s in
                  ["devstorage.full_control", "datastore", "userinfo.email",
                   "firebase.database", "cloud-platform"])

DEFAULT_KEY = (ROOT / "src/network" /
               "esp32-electricity-counter-firebase-adminsdk-fbsvc-d7177737f0.json")
# arduino-cli is not on PATH in this environment.
ARDUINO_CLI = (Path.home() / "apps/arduino-ide/extracted/resources/app/lib/backend/"
               "resources/arduino-cli")
SITE_ID = "esp32-electricity-counter"
SITE_URL = "https://esp32-electricity-counter.web.app"


def say(msg):
    print(" ... " + msg, flush=True)


def ok(fmt, *a):
    print("OK    " + (fmt % a), flush=True)


def b64(s: bytes) -> str:
    import base64
    return base64.urlsafe_b64encode(s).rstrip(b"=").decode()


def get_access_token(key_path):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    with open(key_path) as f:
        creds = json.load(f)
    iat, exp = int(time.time()), int(time.time()) + 3600
    header = b64(json.dumps({"alg": "RS256", "typ": "JWT"}).encode())
    payload = b64(json.dumps({
        "iss": creds["client_email"], "sub": creds["client_email"],
        "aud": "https://oauth2.googleapis.com/token",
        "scope": SCOPES, "iat": iat, "exp": exp}).encode())
    key = serialization.load_pem_private_key(creds["private_key"].encode(), None)
    sig = b64(key.sign((header + "." + payload).encode(),
                       padding.PKCS1v15(), hashes.SHA256()))
    body = urllib.parse.urlencode({
        "grant_type": "urn:ietf:params:oauth:grant-type:jwt-bearer",
        "assertion": header + "." + payload + "." + sig}).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=body)
    with urllib.request.urlopen(req) as r:
        return json.load(r)["access_token"]


def _v4_only():
    """Resolve only IPv4 for this request. Google services all have A records,
    and on this box the AAAA records exist but have no route, so urllib
    occasionally dies with 'Temporary failure in name resolution'."""
    real = socket.getaddrinfo

    def v4(host, port, *a, **k):
        k.pop("family", None)
        args = [host, port, socket.AF_INET]
        if a:
            args.extend(a[1:])  # drop the positional family arg
        return real(*args, **k)

    return real, v4


def rest(url, token, method="GET", data=None, ctype="application/json"):
    """Google/REST call with the OAuth2 access token as `Authorization: Bearer`.
    For RTDB this requires a service-account token minted with the *full* scope
    list (SCOPES); a narrow-scope token is not recognized as admin and RTDB
    simply rejects the write."""
    body = None
    headers = {"Authorization": "Bearer " + token}
    if data is not None:
        body = data if isinstance(data, bytes) else json.dumps(data).encode()
        headers["Content-Type"] = ctype if isinstance(data, bytes) else "application/json"
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    opener = urllib.request.build_opener()
    real, v4 = _v4_only()
    socket.getaddrinfo = v4
    try:
        with opener.open(req, timeout=40) as r:
            txt = r.read().decode()
            return r.status, (json.loads(txt) if txt and txt[0] in "[{" else txt)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")
    finally:
        socket.getaddrinfo = real


def rtdb_get(db, path, token):
    for _ in range(4):
        try:
            return rest("https://{0}{1}".format(db, path), token)
        except (urllib.error.URLError, OSError):
            time.sleep(2)  # flaky resolver on the dev box
    raise urllib.error.URLError("RTDB unreachable after retries")


def rtdb_put(db, path, obj, token):
    return rest("https://{0}{1}".format(db, path), token, method="PUT", data=obj)


def rtdb_put_json(db, path, json_str, token):
    """PUT a raw JSON document (e.g. a huge base64 string literal) to RTDB."""
    return rest("https://{0}{1}".format(db, path), token, method="PUT",
                data=json_str.encode(), ctype="application/json")


def publish_firmware_to_rtdb(binp, dev, token, db):
    """Store the firmware binary in RTDB as the library's setFile base64 format.

    The device's Firebase.RTDB.downloadOTA reads this node and flashes it
    through Update, all on the same fbdo TLS session. The stored value is a
    JSON string of the form "file,base64,<b64>" (or "File,base64,/"fIle..."
    with pad-length encoded into the signature), exactly what the library
    writes via setFile/pushFile.

    Path: /devices/<id>/firmware
    """
    import base64

    raw = binp.read_bytes()
    b64data = base64.b64encode(raw).decode()

    # Pad-length encoded signature: setFile writes
    #   n%3==0 -> "file,base64,   n%3==2 -> "File,base64,   n%3==1 -> "fIle,base64,
    pad = (3 - len(raw) % 3) % 3
    if pad == 1:
        sig = '"File,base64,'
    elif pad == 2:
        sig = '"fIle,base64,'
    else:
        sig = '"file,base64,'

    # The raw JSON document (matches what setFile PUTs): a single JSON string
    # starting with the signature quote and ending with a closing quote.
    blob = sig + b64data + '"'

    code, resp = rest("https://{0}{1}".format(db, dev_path(dev, "firmware.json")),
                      token, method="PUT", data=blob.encode(),
                      ctype="application/json")
    if code != 200:
        sys.exit("FAIL    RTDB firmware write HTTP {0}: {1}".format(code, resp))
    ok("published firmware to %s (%d KiB base64)",
       dev_path(dev, "firmware"), len(b64data) // 1024)


def bar(pct):
    filled = int(pct / 100 * 20)
    return "#" * filled + "-" * (20 - filled)


def dev_path(dev, node):
    """RTDB paths are namespaced per board so multiple boards can share one
    database without clobbering each other."""
    return "/devices/%s/%s" % (dev, node)


def poll_status(args, token, dev):
    """Tail /devices/<id>/ota/status like an Arduino IDE progress bar.

    NOTE: the device suspends Firebase during the download (to free the ~150KB
    SSL heap), so /ota/status is only written before/after the transfer. If the
    status node never appears, don't hang forever — fall through to the boot
    check, which is the authoritative success signal."""
    last = ""
    last_seen = time.time()
    while True:
        code, st = rtdb_get(args.db, dev_path(dev, "ota/status.json"), token)
        if code == 200 and isinstance(st, dict) and st.get("state"):
            last_seen = time.time()
            state, prog = st["state"], int(st.get("progress") or 0)
            if state != last:
                say("state: " + state)
                last = state
            if state == "downloading":
                sys.stdout.write("\r    {0} {1:3d}%".format(bar(prog), prog))
                sys.stdout.flush()
            elif state == "done":
                sys.stdout.write("\n")
                ok("device flashed. awaiting reboot.")
                return
            elif state.startswith("failed"):
                sys.exit("FAIL    device reported: " + state)
        elif time.time() - last_seen > 45:
            # No status channel at all (OTA suspends Firebase) — let the boot
            # check decide. Serial shows the download progress instead.
            say("no status channel (device suspends Firebase during OTA) - "
                "watching for reboot")
            return
        time.sleep(0.8)


def wait_boot(args, token, version, dev):
    say("waiting for device to boot into " + version + " ...")
    deadline = time.time() + 90
    while time.time() < deadline:
        code, d = rtdb_get(args.db, dev_path(dev, "latest.json"), token)
        if code == 200 and isinstance(d, dict) and d.get("firmwareVersion") == version:
            ok("device ONLINE on %s (epoch %s)", version, d.get("epoch"))
            return
        time.sleep(3)
    sys.exit("FAIL    could not confirm new version after 90s (check the device)")


def read_version():
    """Pull FIRMWARE_VERSION out of src/config.h — the single source of truth.
    The firmware reports this exact string to RTDB, so the published tag has to
    match it or the boot-check would never pass."""
    for line in (ROOT / "src" / "config.h").read_text().splitlines():
        m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', line)
        if m:
            return m.group(1)
    sys.exit("FATAL    FIRMWARE_VERSION not found in src/config.h")


def main():
    ap = argparse.ArgumentParser(
        description="Flash ESP32 firmware over Firebase (OTA from anywhere). "
                    "Run from the project root:  python3 tools/ota_upload.py  "
                    "(version is read from src/config.h)")
    ap.add_argument("version", nargs="?", default=None,
                    help="version tag; defaults to FIRMWARE_VERSION in src/config.h")
    ap.add_argument("--bin", default=None, help="path to a prebuilt .ino.bin (skip compile)")
    ap.add_argument("--key", default=str(DEFAULT_KEY))
    ap.add_argument("--db", default="esp32-electricity-counter-default-rtdb.firebaseio.com")
    ap.add_argument("--no-boot-check", action="store_true")
    ap.add_argument("--device", default="esp-000000",
                    help="target board id (/devices/<id>/ota). Must match the "
                         "chip-unique id shown in the device boot log.")
    args = ap.parse_args()

    version = args.version or read_version()
    if args.version:
        say("using explicit version %s (firmware reports %s; use --no-boot-check "
            "if they differ)" % (args.version, read_version()))

    if not os.path.exists(args.key):
        sys.exit("FATAL    service-account key not found: " + args.key)

    say("authenticating with Firebase service account")
    token = get_access_token(args.key)
    say("signed in")

    if args.bin:
        if not os.path.exists(args.bin):
            sys.exit("FATAL    bin not found: " + args.bin)
        binp = Path(args.bin)
    else:
        say("compiling firmware " + version)
        build = Path("/tmp/ota-" + version)
        rc = subprocess.run(
            [os.environ.get("ARDUINO", str(ARDUINO_CLI)), "compile", "--fqbn", FQBN,
             "--build-property", "compiler.cpp.extra_flags=-DENABLE_OTA_FIRMWARE_UPDATE",
             "--build-path", str(build), "."], cwd=ROOT).returncode
        if rc != 0:
            sys.exit("FATAL    compile failed")
        matches = [p for p in build.rglob("*") if str(p).endswith(".ino.bin")]
        if not matches:
            sys.exit("FATAL    no .ino.bin produced")
        binp = matches[0]

    md5 = hashlib.md5(binp.read_bytes()).hexdigest()
    say("md5 = " + md5)

    # Firmware blob to RTDB, then the /ota trigger node.
    publish_firmware_to_rtdb(binp, args.device, token, args.db)

    say("triggering OTA via RTDB /devices/%s/ota ..." % args.device)
    code, resp = rtdb_put(args.db, dev_path(args.device, "ota.json"),
                          {"version": version, "md5": md5,
                           "ts": int(time.time())}, token)
    if code != 200:
        sys.exit("FAIL    RTDB trigger HTTP {0}: {1}".format(code, resp))
    ok("triggered update to %s on %s (device flashes itself anywhere in the world)",
       version, args.device)

    poll_status(args, token, args.device)
    if not args.no_boot_check:
        wait_boot(args, token, version, args.device)


if __name__ == "__main__":
    main()
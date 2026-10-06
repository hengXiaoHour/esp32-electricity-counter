#!/usr/bin/env python3
"""
build.py — stamp, build both targets, and push to GitHub for OTA.

Counter equivalent of cloud-ota's build.py: one command takes the repo from
the current FIRMWARE_VERSION to a new release both boards can `update` to.

    python3 scripts/build.py                  # bump patch, build, release
    python3 scripts/build.py --version 3.3.0  # explicit version
    python3 scripts/build.py --skip-push      # stamp + build only, no git/push

What it does:
  1. Stamps src/config.h FIRMWARE_VERSION (the bump IS the commit).
  2. Tracks scripts/mock_device.py FW_VERSION to the same stamp.
  3. Compiles BOTH targets via scripts/build.sh (S3 + classic min_spiffs).
  4. Copies the two app bins to ./build/esp32-{s3,classic}-X.Y.Z.bin.
  5. Rewrites version.json (the file `update` polls).
  6. Commits stamp + manifest + mock, pushes, creates a GitHub release
     holding both .bins (unless --skip-push).

The serial-first flow stays intact: after the release, any board on STA
prints the new version on `update` and stages its per-chip asset.
"""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).parent.parent
CONFIG = ROOT / "src/config.h"
MOCK = ROOT / "scripts/mock_device.py"
VERSION_JSON = ROOT / "version.json"
BUILD_DIR = ROOT / "build"


def run(cmd, check=True):
    print("$ %s" % " ".join(cmd))
    result = subprocess.run(cmd, cwd=ROOT)
    if check and result.returncode != 0:
        sys.exit(result.returncode)
    return result


def get_version():
    m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', CONFIG.read_text())
    return m.group(1) if m else "0.0.0"


def set_stamp(path, pattern, replacement):
    text = path.read_text()
    new, n = re.subn(pattern, replacement, text, count=1)
    if n != 1:
        print("ERROR: stamp hit %d lines in %s (want exactly 1)" % (n, path))
        sys.exit(1)
    path.write_text(new)


def bump_patch(v):
    try:
        a, b, c = [int(x) for x in v.split(".")]
        return "%d.%d.%d" % (a, b, c + 1)
    except (ValueError, AttributeError):
        return v


def find_app_bin(outdir, sketch="esp32-electricity-counter.ino.bin"):
    exact = outdir / sketch
    if exact.exists():
        return exact
    cands = [p for p in outdir.glob("*.bin")
             if "merged" not in p.name and "bootloader" not in p.name
             and "partitions" not in p.name]
    return cands[0] if cands else None


def repo_slug():
    try:
        remote = subprocess.check_output(
            ["git", "remote", "get-url", "origin"], text=True).strip()
        m = re.search(r"github\.com[:/](.+?)/(.+?)(\.git)?$", remote)
        if m:
            return "%s/%s" % (m.group(1), m.group(2))
    except subprocess.CalledProcessError:
        pass
    return "hengXiaoHour/esp32-electricity-counter"


def main():
    parser = argparse.ArgumentParser(description="Stamp + build + release OTA")
    parser.add_argument("--version",
                        help="New version x.y.z (default: bump patch)")
    parser.add_argument("--skip-push", action="store_true",
                        help="Stamp + build only, no git push / release")
    args = parser.parse_args()

    if shutil.which("arduino-cli") is None and shutil.which(
            str(pathlib.Path.home() / ".local/bin/arduino-cli")) is None:
        print("ERROR: arduino-cli not found (scripts/build.sh needs it).")
        sys.exit(1)

    cur = get_version()
    suggested = bump_patch(cur)
    if args.version:
        new_ver = args.version
    else:
        try:
            new_ver = input(
                "Current FIRMWARE_VERSION=%s -> New version [%s]: "
                % (cur, suggested)).strip() or suggested
        except EOFError:
            new_ver = suggested
    if not re.match(r"^\d+\.\d+\.\d+$", new_ver):
        print("ERROR: version must be x.y.z, got '%s'" % new_ver)
        sys.exit(1)

    print("\n=== Building %s -> %s ===" % (cur, new_ver))

    # --- 1. Stamp ------------------------------------------------------
    set_stamp(CONFIG,
              r'#define\s+FIRMWARE_VERSION\s+"[^"]+"',
              '#define FIRMWARE_VERSION "%s"' % new_ver)
    print("stamped src/config.h FIRMWARE_VERSION=%s" % new_ver)

    # --- 2. Mock tracks the stamp (serial-first verbs are faked there) --
    set_stamp(MOCK,
              r'FW_VERSION\s*=\s*"[^"]+"',
              'FW_VERSION = "%s"' % new_ver)
    print("tracked scripts/mock_device.py FW_VERSION=%s" % new_ver)

    # --- 3. Compile both targets via the one supported entry point -----
    s3_dir = BUILD_DIR / "s3"
    classic_dir = BUILD_DIR / "classic"
    run(["scripts/build.sh", "--output-dir", str(s3_dir)])
    run(["scripts/build.sh", "--classic", "--output-dir", str(classic_dir)])

    s3_bin = find_app_bin(s3_dir)
    classic_bin = find_app_bin(classic_dir)
    if not s3_bin or not classic_bin:
        print("ERROR: app .bin missing (s3=%s classic=%s)" % (s3_bin, classic_bin))
        sys.exit(1)
    print("s3 bin: %s (%d bytes)" % (s3_bin, s3_bin.stat().st_size))
    print("classic bin: %s (%d bytes)" % (classic_bin, classic_bin.stat().st_size))

    # --- 4. Release assets with the exact names `update` expects --------
    s3_asset = BUILD_DIR / ("esp32-s3-%s.bin" % new_ver)
    classic_asset = BUILD_DIR / ("esp32-classic-%s.bin" % new_ver)
    shutil.copy(s3_bin, s3_asset)
    shutil.copy(classic_bin, classic_asset)
    print("assets: %s, %s" % (s3_asset.name, classic_asset.name))

    # --- 5. Manifest ----------------------------------------------------
    # Tag carries the v prefix (v3.2.14 precedent) and the manifest URLs must
    # match it EXACTLY: 3.2.14 shipped no-v URLs against a v-tag and every
    # staged download 404'd. This rule is load-bearing, not cosmetic.
    slug = repo_slug()
    tag = "v%s" % new_ver
    data = {
        "version": new_ver,
        "s3_bin_url": "https://github.com/%s/releases/download/%s/%s"
                      % (slug, tag, s3_asset.name),
        "classic_bin_url": "https://github.com/%s/releases/download/%s/%s"
                           % (slug, tag, classic_asset.name),
        "notes": "Release %s" % new_ver,
    }
    VERSION_JSON.write_text(json.dumps(data, indent=2) + "\n")
    print(VERSION_JSON.read_text())

    if args.skip_push:
        print("--skip-push: stopped before git. Push manually when ready.")
        return

    # --- 6. Commit + push + release -------------------------------------
    if shutil.which("gh") is None:
        print("ERROR: gh CLI not found; commit/push manually.")
        sys.exit(1)
    run(["git", "add", "src/config.h", "scripts/mock_device.py", "version.json"])
    subprocess.run(["git", "commit", "-m", "chore: bump OTA to %s" % new_ver],
                   cwd=ROOT)
    run(["git", "push", "origin", "main"])
    subprocess.run(["git", "tag", "-d", "v%s" % new_ver], cwd=ROOT,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    result = subprocess.run(
        ["gh", "release", "create", "v%s" % new_ver, str(s3_asset),
         str(classic_asset), "--title", "v%s" % new_ver,
         "--notes", "OTA %s" % new_ver, "--target", "main"], cwd=ROOT)
    if result.returncode != 0:
        print("gh release failed (maybe tag exists). Trying upload...")
        run(["gh", "release", "upload", "v%s" % new_ver, str(s3_asset),
             str(classic_asset), "--clobber"], check=False)
    print("\nDone! Release v%s ready: https://github.com/%s/releases/tag/v%s"
          % (new_ver, slug, new_ver))
    print("Boards on STA will offer it on the next `update` check.")


if __name__ == "__main__":
    main()

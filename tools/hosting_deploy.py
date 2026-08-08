#!/usr/bin/env python3
"""Deploy the frontend/* PWA to Firebase Hosting via the Hosting REST API,
using the Firebase Admin service account (no firebase CLI / browser login).

Flow (matches https://firebase.google.com/docs/hosting/api-deploy):
  1. versions.create      -> version id (status CREATED)
  2. hash each file: sha256(gzip(file))
  3. versions:populateFiles -> list of hashes Firebase does not have yet
  4. upload gzipped bytes to uploadUrl/<hash> for each missing hash
  5. versions.patch status=FINALIZED
  6. releases.create      -> live on SITE_URL

Usage:
    python3 tools/hosting_deploy.py            # deploy frontend/ to the live site
    python3 tools/hosting_deploy.py --preview  # deploy to a --preview channel
"""
import gzip
import hashlib
import json
import os
import socket
import sys
import urllib.request
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from ota_upload import get_access_token, rest  # noqa: E402

SITE_ID = "esp32-electricity-counter"
PROJECT = "esp32-electricity-counter"
KEY = ROOT / "src/network/esp32-electricity-counter-firebase-adminsdk-fbsvc-d7177737f0.json"
PUBLIC = ROOT / "frontend"

HOSTING = "https://firebasehosting.googleapis.com/v1beta1"
UPLOAD = "https://upload-firebasehosting.googleapis.com/upload"

# Mirrors firebase.json (rewrites + headers) so SPA routing stays intact.
CONFIG = {
    "rewrites": [{"glob": "**", "path": "/index.html"}],
    "headers": [
        {"glob": "/sw.js", "headers": {"Cache-Control": "no-cache"}},
        {"glob": "/manifest.json", "headers": {"Cache-Control": "no-cache"}},
    ],
}


def _v4_only():
    real = socket.getaddrinfo

    def v4(host, port, *a, **k):
        k.pop("family", None)
        args = [host, port, socket.AF_INET]
        if a:
            args.extend(a[1:])
        return real(*args, **k)

    return real, v4


def http_with_token(url, token, data, ctype="application/octet-stream"):
    req = urllib.request.Request(url, data=data,
                                 headers={"Authorization": "Bearer " + token,
                                          "Content-Type": ctype},
                                 method="POST")
    opener = urllib.request.build_opener()
    real, v4 = _v4_only()
    socket.getaddrinfo = v4
    try:
        with opener.open(req, timeout=90) as r:
            return r.read()
    except Exception as e:
        raise e
    finally:
        socket.getaddrinfo = real


def main():
    token = get_access_token(str(KEY))
    raw = path.read_bytes()
    gz = gzip.compress(raw, compresslevel=9)
    return hashlib.sha256(gz).hexdigest(), gz


def gz_hash(path):
    raw = path.read_bytes()
    gz = gzip.compress(raw, compresslevel=9)
    return hashlib.sha256(gz).hexdigest(), gz


def collect_files():
    files = {}
    for p in sorted(PUBLIC.rglob("*")):
        if p.is_file() and p.name != "config.example.js":
            rel = "/" + p.relative_to(PUBLIC).as_posix()
            h, _ = gz_hash(p)
            files[rel] = (h, p)
    return files


def main():
    token = get_access_token(str(KEY))
    files = collect_files()

    # 1. create version
    code, ver = rest(f"{HOSTING}/sites/{SITE_ID}/versions", token,
                     method="POST", data={"config": CONFIG})
    if code != 200:
        print("create version failed:", code, str(ver)[:300])
        sys.exit(1)
    vname = ver["name"]
    print("version:", vname)

    # 2. populate files (map path -> sha256gz)
    body = {"files": {rel: h for rel, (h, _) in files.items()}}
    code, pop = rest(f"{HOSTING}/{vname}:populateFiles", token,
                     method="POST", data=body)
    if code not in (200, 202):
        print("populateFiles failed:", code, str(pop)[:300])
        sys.exit(1)

    # 3. upload any hashes Firebase does not yet have
    for h in pop.get("uploadRequiredHashes", []):
        # find the matching local path (hash is unique)
        rel = next((r for r, (hh, _) in files.items() if hh == h), None)
        if rel is None:
            continue
        _, gz = gz_hash(files[rel][1])
        url = f"{UPLOAD}/sites/{SITE_ID}/versions/{vname.split('/')[-1]}/files/{h}"
        try:
            http_with_token(url, token, gz)
            print("uploaded:", rel, f"({len(gz)} gz bytes)")
        except Exception as e:
            print("upload failed", rel, e)
            sys.exit(1)

    # 4. finalize
    code, _ = rest(f"{HOSTING}/{vname}?updateMask=status", token,
                   method="PATCH", data={"status": "FINALIZED"})
    if code != 200:
        print("finalize failed:", code)
        sys.exit(1)

    # 5. release
    code, rel = rest(f"{HOSTING}/sites/{SITE_ID}/releases?versionName={urllib.parse.quote(vname)}", token,
                     method="POST",
                     data={"message": "hosting_deploy.py"})
    print("release:", code, rel.get("name") if isinstance(rel, dict) else rel)
    if code in (200, 201):
        print(f"Live: https://{SITE_ID}.web.app")
    else:
        print(str(rel)[:300])


if __name__ == "__main__":
    main()
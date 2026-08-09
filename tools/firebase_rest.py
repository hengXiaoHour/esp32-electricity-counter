#!/usr/bin/env python3
"""Firebase REST helpers — token minting and HTTP wrappers."""
import json
import subprocess
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

DEFAULT_KEY = (ROOT / "src/network" /
               "esp32-electricity-counter-firebase-adminsdk-fbsvc-d7177737f0.json")


def b64(s: bytes) -> str:
    import base64
    return base64.urlsafe_b64encode(s).rstrip(b"=").decode()


def get_access_token(key_path):
    """Mint an OAuth2 access token from a service-account JSON key."""
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding

    with open(key_path) as f:
        creds = json.load(f)

    import time
    iat, exp = int(time.time()), int(time.time()) + 3600
    header = b64(json.dumps({"alg": "RS256", "typ": "JWT"}).encode())
    payload = b64(json.dumps({
        "iss": creds["client_email"], "sub": creds["client_email"],
        "aud": "https://oauth2.googleapis.com/token",
        "scope": " ".join("https://www.googleapis.com/auth/" + s for s in
                          ["firebase.database", "cloud-platform",
                           "userinfo.email", "devstorage.full_control"]),
        "iat": iat, "exp": exp
    }).encode())
    key = serialization.load_pem_private_key(creds["private_key"].encode(), None)
    sig = b64(key.sign((header + "." + payload).encode(),
                       padding.PKCS1v15(), hashes.SHA256()))
    assertion = header + "." + payload + "." + sig

    body = urllib.parse.urlencode({
        "grant_type": "urn:ietf:params:oauth:grant-type:jwt-bearer",
        "assertion": assertion
    }).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=body)
    with urllib.request.urlopen(req) as r:
        return json.load(r)["access_token"]


def rest(url, token, method="GET", data=None, ctype="application/json"):
    """HTTP wrapper. Returns (status_code, parsed_json_or_None)."""
    req = urllib.request.Request(url, method=method)
    req.add_header("Authorization", f"Bearer {token}")
    if data is not None:
        if isinstance(data, dict):
            data = json.dumps(data).encode()
        req.add_header("Content-Type", ctype)
        req.data = data
    try:
        with urllib.request.urlopen(req) as r:
            body = r.read().decode()
            return r.status, (json.loads(body) if body else None)
    except urllib.error.HTTPError as e:
        body = e.read().decode()
        try:
            return e.code, json.loads(body)
        except Exception:
            return e.code, None

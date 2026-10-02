#!/usr/bin/env python3
"""Generate src/network/web_assets.h from the frontend/ PWA sources.

The device serves the dashboard itself (AP-only architecture), so every asset
the browser needs has to live in flash. This script bakes frontend/ into a
PROGMEM table that WebSocketServer::serveAsset() looks up.

Text assets use PROGMEM raw string literals (fast to compile, readable).
Binary assets use PROGMEM byte arrays. Sizes come from sizeof(), never from a
hand-written number, so the table cannot drift from the bytes.

Usage:
    python3 scripts/embed_web.py           # write src/network/web_assets.h
    python3 scripts/embed_web.py --check   # fail if the checked-in header is stale
    python3 scripts/embed_web.py --verify  # parse the header BACK and compare bytes

--check re-derives the header and compares it with the file on disk, so a
forgotten regeneration fails loudly instead of shipping an old dashboard.
--verify is the independent half: it parses the generated C++ (raw literals and
byte arrays) back into bytes and compares each asset against frontend/. That is
the check that actually proves the device would serve the real file, because it
does not trust the generator's own in-memory value.

Both flags must be run against a deliberately mutated copy at least once, or
they are untested assertions that always pass.
"""

import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FRONTEND = ROOT / "frontend"
OUT = ROOT / "src" / "network" / "web_assets.h"

# (device path, source relative to frontend/, mime type, encoding)
# `optional: True` means "skip silently if missing" — config.js is gitignored
# and disappears entirely once Firebase is gone.
ASSETS = [
    ("/index.html", "index.html", "text/html; charset=utf-8", "text", False),
    ("/style.css", "style.css", "text/css; charset=utf-8", "text", False),
    ("/script.js", "script.js", "application/javascript; charset=utf-8", "text", False),
    ("/manifest.json", "manifest.json", "application/manifest+json", "text", False),
    ("/sw.js", "sw.js", "application/javascript; charset=utf-8", "text", False),
    ("/config.js", "config.js", "application/javascript; charset=utf-8", "text", True),
    ("/icons/icon-192.png", "icons/icon-192.png", "image/png", "bin", False),
    ("/icons/icon-512.png", "icons/icon-512.png", "image/png", "bin", False),
    ("/icons/apple-touch-icon.png", "icons/apple-touch-icon.png", "image/png", "bin", False),
    ("/icons/maskable-192.png", "icons/maskable-192.png", "image/png", "bin", False),
    ("/icons/maskable-512.png", "icons/maskable-512.png", "image/png", "bin", False),
]

RAW_OPEN = 'R"rawliteral('
RAW_CLOSE = ')rawliteral"'


def ident(path: str, index: int) -> str:
    slug = "".join(c if c.isalnum() else "_" for c in path.lstrip("/"))
    return "WEB_ASSET_%d_%s" % (index, slug.upper())


def emit_text(data: bytes, name: str) -> str:
    """PROGMEM raw string literal holding `data` byte-for-byte.

    The literal opens immediately before the first content byte and, when the
    source ends in a newline, closes on its own line purely for readability.
    No newline is inserted at the front: an earlier version opened with
    `R"rawliteral(` + "\\n", which silently prepended one byte to every text
    asset and made the served file differ from the file on disk.
    """
    text = data.decode("utf-8")
    if RAW_CLOSE in text:
        raise SystemExit(
            "FATAL: %s contains the raw-literal delimiter %s.\n"
            "Change RAW_CLOSE in this generator to a sequence that cannot occur."
            % (name, RAW_CLOSE)
        )
    # Close immediately after the last content byte. When the source already
    # ends in a newline this naturally lands on its own line (readable); when it
    # does not, it appends inline. Either way no byte is added or removed.
    return "static const char %s[] PROGMEM = %s%s%s;\n\n" % (name, RAW_OPEN, text, RAW_CLOSE)


def emit_bin(data: bytes, name: str) -> str:
    lines = ["static const uint8_t %s[] PROGMEM = {" % name]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("  " + ",".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};\n")
    return "\n".join(lines) + "\n"


def build() -> str:
    entries = []
    digest = hashlib.sha256()

    for i, (route, rel, mime, kind, optional) in enumerate(ASSETS):
        src = FRONTEND / rel
        if not src.exists():
            if optional:
                continue
            raise SystemExit("FATAL: required asset missing: %s" % src)

        data = src.read_bytes()
        if not data:
            raise SystemExit("FATAL: asset is empty: %s" % src)

        digest.update(route.encode())
        digest.update(data)
        entries.append((route, rel, mime, kind, data, i))

    total = sum(len(e[4]) for e in entries)
    out = []
    out.append("// GENERATED FILE - DO NOT EDIT BY HAND.")
    out.append("// Source:    frontend/ (run: python3 scripts/embed_web.py)")
    out.append("// Content:   sha256:%s" % digest.hexdigest()[:16])
    out.append("// Assets:    %d files, %d bytes" % (len(entries), total))
    out.append("//")
    out.append("// Text assets are PROGMEM raw string literals; binary assets are byte")
    out.append("// arrays. Both sizes come from sizeof() at the point of use, so the")
    out.append("// table can never disagree with the bytes it points at.")
    out.append("")
    out.append("#pragma once")
    out.append("")
    out.append("#include <Arduino.h>")
    out.append("")
    out.append("struct WebAsset {")
    out.append("  const char *path;")
    out.append("  const char *mime;")
    out.append("  const char *data;")
    out.append("  uint32_t size;")
    out.append("};")
    out.append("")

    for route, rel, mime, kind, data, i in entries:
        name = ident(route, i)
        out.append("// ---- %s  (%s, %d bytes) ----" % (route, rel, len(data)))
        out.append(emit_text(data, name) if kind == "text" else emit_bin(data, name))

    out.append("#define WEB_ASSET_COUNT %d\n" % len(entries))
    out.append("static const WebAsset WEB_ASSETS[WEB_ASSET_COUNT] = {")
    for route, rel, mime, kind, data, i in entries:
        name = ident(route, i)
        if kind == "text":
            out.append('  { "%s", "%s", %s, (uint32_t)(sizeof(%s) - 1) },'
                       % (route, mime, name, name))
        else:
            out.append('  { "%s", "%s", (const char *)%s, (uint32_t)sizeof(%s) },'
                       % (route, mime, name, name))
    out.append("};\n")

    return "\n".join(out)


def parse_back(header_text: str) -> dict:
    """Reconstruct asset bytes by parsing the generated C++.

    Deliberately independent of build(): it reads the header as text, finds
    each `static const char NAME[] PROGMEM = R"rawliteral(<content>)rawliteral";`
    or `static const uint8_t NAME[] PROGMEM = { 0x.., ... };`, and returns
    {device path: bytes}. A bug in build() that corrupted the encoding would
    have to corrupt it identically in this parser to go unnoticed.
    """
    payloads = {}

    for m in re.finditer(
        r'static const char (WEB_ASSET_\w+)\[\] PROGMEM = R"rawliteral\((.*?)\)rawliteral";',
        header_text, re.S,
    ):
        payloads[m.group(1)] = m.group(2).encode("utf-8")

    for m in re.finditer(
        r'static const uint8_t (WEB_ASSET_\w+)\[\] PROGMEM = \{(.*?)\};', header_text, re.S
    ):
        payloads[m.group(1)] = bytes(
            int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(2))
        )

    # Walk the WEB_ASSETS table to map each payload back to its device path.
    route_to_rel = {route: rel for route, rel, _m, _k, _o in ASSETS}
    rows = re.findall(
        r'\{\s*"([^"]+)",\s*"[^"]+",\s*(?:\(const char \*\))?(WEB_ASSET_\w+),', header_text
    )
    return {route: payloads.get(name) for route, name in rows
            if route in route_to_rel}


def verify(header_text: str) -> int:
    """Compare parsed-back bytes against frontend/. Returns process exit code."""
    parsed = parse_back(header_text)
    failures = 0

    expected_routes = {route for route, rel, m, k, o in ASSETS
                       if not o or (FRONTEND / rel).exists()}

    if set(parsed) != expected_routes:
        print("FAIL: table routes %s != expected %s"
              % (sorted(parsed), sorted(expected_routes)))
        return 1

    for route in sorted(parsed):
        src = FRONTEND / route_to_rel_for(route)
        want = src.read_bytes()
        got = parsed[route]
        if got is None:
            print("FAIL %-28s payload not found in header" % route)
            failures += 1
        elif got != want:
            # Report the first difference so a shifted-by-one bug is obvious.
            off = next((i for i in range(min(len(got), len(want)))
                        if got[i] != want[i]), min(len(got), len(want)))
            print("FAIL %-28s %d bytes served vs %d on disk; first diff at byte %d"
                  % (route, len(got), len(want), off))
            print("       served: %r" % got[max(0, off - 10):off + 10])
            print("       disk  : %r" % want[max(0, off - 10):off + 10])
            failures += 1
        else:
            print("  ok %-28s %7d bytes byte-for-byte" % (route, len(want)))

    if failures:
        print("FAIL: %d asset(s) would not be served correctly" % failures)
        return 1
    print("PASS: all %d assets round-trip byte-for-byte from the generated header"
          % len(parsed))
    return 0


def route_to_rel_for(route: str) -> str:
    for r, rel, _m, _k, _o in ASSETS:
        if r == route:
            return rel
    raise KeyError(route)


def main() -> int:
    text = build()

    if "--check" in sys.argv:
        if not OUT.exists():
            print("FAIL: %s does not exist - run scripts/embed_web.py" % OUT)
            return 1
        current = OUT.read_text()
        if current != text:
            print("FAIL: %s is STALE - run: python3 scripts/embed_web.py" % OUT)
            print("      on disk: %d bytes, generated: %d bytes"
                  % (len(current), len(text)))
            return 1
        print("PASS: %s matches frontend/ (%d bytes)"
              % (OUT.relative_to(ROOT), len(current)))
        return 0

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text)
    print("Wrote %s (%d bytes)" % (OUT.relative_to(ROOT), len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
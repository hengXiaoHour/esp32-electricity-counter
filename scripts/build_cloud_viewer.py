#!/usr/bin/env python3
"""Build cloud-viewer/ from frontend/ so both share the SAME UI.

The cloud dashboard is the local dashboard with a different transport:
  frontend/  : WebSocket to the board (location.host/ws), PIN auth.
  cloud-viewer/: Firebase RTDB (/devices/<MAC>/latest + /cmd + /ack),
               Google-admin auth, public read-only.

To keep the look identical without drifting, this script COPIES the
rendering files verbatim and applies only small cloud patches:
  index.html : + Firebase SDK, + Cloud device panel first in Settings
               (picker / freshness / Google sign-in; header is "Connected"),
               Admin panel dropped, hints stripped, panels reordered;
  style.css  : frontend verbatim + cloud-bar additions appended
  script.js  : exact copy of frontend/script.js (rendering, cards, charts,
               events, settings, console). cloud.js overrides the TRANSPORT
               functions after load (connect/send/auth), never the rendering.
  manifest.json, icons/, sw.js : copied (sw cache name bumped for cloud).

Re-run after any frontend change:
    python3 scripts/build_cloud_viewer.py
"""
import re
import shutil
import sys
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FRONT = ROOT / "frontend"
CLOUD = ROOT / "cloud-viewer"

FIREBASE_SDK = """<script src="https://www.gstatic.com/firebasejs/10.12.2/firebase-app-compat.js"></script>
<script src="https://www.gstatic.com/firebasejs/10.12.2/firebase-auth-compat.js"></script>
<script src="https://www.gstatic.com/firebasejs/10.12.2/firebase-database-compat.js"></script>
"""

CLOUD_BAR = """    <!-- Cloud device panel: lives in Settings so the header status bar
         stays identical to the board-served dashboard ("Connected"). -->
    <div id="cloudBar" class="panel-box cloud-bar">
      <div class="panel-header"><h3>Cloud device</h3></div>
      <div class="pin-badge-row">
        <span id="roleBadge" class="role-badge role-guest">Viewer &mdash; read-only</span>
      </div>
      <div class="cal-row-single">
        <label for="devicePicker">Device:</label>
        <div class="cal-field">
          <select id="devicePicker" class="cloud-select" aria-label="device"></select>
        </div>
      </div>
      <div class="cal-row-single">
        <label>Freshness:</label>
        <div class="cal-field">
          <span class="mono conn-txt"><span id="cloudDot" class="led" title="cloud freshness"></span> <span id="cloudFresh">connecting&hellip;</span></span>
        </div>
        <span id="staleTag" class="stale-tag hidden">OFFLINE</span>
      </div>
      <div class="cal-row-single">
        <label>Google:</label>
        <div class="cal-field">
          <span id="userEmail" class="mono dim"></span>
        </div>
        <button class="btn-sm" id="authBtn">Sign in with Google</button>
      </div>
      <div id="cloudCmdStatus" class="mono dim cloud-status">No command sent yet.</div>
    </div>
"""

def _panel_spans(text):
    """Spans of every top-level <div class="panel-box..."> in `text`."""
    spans = []
    for m in re.finditer(r'<div[^>]*class="panel-box', text):
        oi = m.start()
        depth = 0
        end = -1
        for n in re.finditer(r'</?div\b', text[oi:]):
            if n.group(0) == '<div':
                depth += 1
            else:
                depth -= 1
                if depth == 0:
                    gt = text.find('>', oi + n.end() - 1)
                    end = gt + 1
                    break
        if end > 0:
            spans.append((oi, end))
    return spans


# Settings order for the cloud copy: identity first, then link, uplink
# account, networks, tuning, console, about. The board-PIN Admin panel is
# dropped (its role badge moved into the Cloud device panel above).
PANEL_ORDER = [
    "Cloud device",
    "Connection",
    "Remote Monitoring (cloud)",
    "Home Network (default)",
    "Access Point (fallback)",
    "System Calibration",
    "Device Console",
    "About",
    "Firmware",
]

# Explanatory paragraphs stripped from the cloud copy (minimalist settings;
# the board-served dashboard keeps them). Id-based hints plus the two long
# hint divs without ids, matched by their opening words.
HINT_IDS = ("installHint2", "apHint", "staHint", "cloudHint")
HINT_TEXT_PREFIXES = (
    "Runs the device's diagnostic commands remotely.",
    "ESP32 Counter dashboard",
)


def _reorder_settings(html):
    sec_open = html.find('<section id="page-settings"')
    if sec_open < 0:
        return html
    sec_tag_end = html.find('>', sec_open) + 1
    sec_close = html.find('</section>', sec_tag_end)
    if sec_close < 0:
        return html
    head, body, tail = html[:sec_tag_end], html[sec_tag_end:sec_close], html[sec_close:]
    spans = _panel_spans(body)
    if not spans:
        return html
    panels = []
    for oi, end in spans:
        block = body[oi:end]
        t = re.search(r'<h3>(.*?)</h3>', block)
        panels.append((t.group(1) if t else '', block))
    # Fill gaps (whitespace/comments between panels) stays with the head.
    ordered = []
    by_title = {}
    for title, block in panels:
        by_title.setdefault(title, []).append(block)
    for want in PANEL_ORDER:
        ordered.extend(by_title.pop(want, []))
    # Anything unrecognized (future panels, the dropped Admin PIN last):
    # drop the board-PIN Admin panel, keep anything else in place.
    for title, blocks in by_title.items():
        if title == 'Admin PIN':
            continue
        ordered.extend(blocks)
    new_body = '\n'.join(ordered) + '\n'
    html = head + new_body + tail
    # Strip the explanatory hint paragraphs (cloud copy only). Attribute
    # order varies (installHint2 puts id first), so match by id alone.
    for hid in HINT_IDS:
        html = re.sub(r'<div[^>]*id="%s"[^>]*>[\s\S]*?</div>' % hid, '', html)
    for prefix in HINT_TEXT_PREFIXES:
        html = re.sub(r'<div class="hint">%s[\s\S]*?</div>' % re.escape(prefix),
                      '', html)
    return html

CLOUD_CSS = """
/* ---- Cloud-only additions (device bar, auth, freshness). Everything above
   this line is a verbatim copy of frontend/style.css so the look matches. */
.cloud-bar { margin-bottom: 16px; }
.cloud-row { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.cloud-row + .cloud-row { margin-top: 8px; }
.cloud-select {
  flex: 1; min-width: 160px; height: var(--field-h); padding: 0 10px;
  background: var(--bg-input); border: 1px solid var(--border-2); color: var(--text);
  border-radius: 5px; font-family: var(--font-mono); font-size: 0.85rem; outline: none;
}
.stale-tag {
  font-size: 0.66rem; font-weight: 700; letter-spacing: 0.1em;
  background: var(--warn); color: #000; padding: 2px 8px; border-radius: 3px;
}
.cloud-status { margin-top: 8px; font-size: 0.72rem; white-space: pre-wrap; word-break: break-word; }
#userEmail { flex: 1 1 auto; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
#cloudDot.led-ok { background: var(--ok); box-shadow: 0 0 10px var(--ok); }
#cloudDot.led-stale { background: var(--warn); box-shadow: 0 0 10px var(--warn); }
"""


def patch_index(src: str) -> str:
    html = src
    # Title: same app, cloud copy.
    html = html.replace(
        "<title>ESP32 Counter — Electricity Monitor</title>",
        "<title>ESP32 Counter — Cloud</title>",
    )
    # Firebase SDK before </head>.
    if "firebase-app-compat.js" not in html:
        html = html.replace("</head>", FIREBASE_SDK + "</head>")
    # Connect panel note: cloud has no board WiFi to join.
    html = html.replace(
        """      This page is served by the board itself. Join its WiFi network
      (<span class="mono">ESP32-Elec-Counter</span>) and reload if you are not connected.""",
        """      Cloud mirror of the meter, pushed by the board every second over home WiFi.
      It connects automatically &mdash; switch devices in Settings &mdash; no board WiFi needed.""",
    )
    html = html.replace(
        "&#9654; Reconnect", "&#9654; Retry",
    )
    # (Sidebar footer went away entirely - no Admin PIN / Sign Out buttons.)
    # Connection panel: Board and Status said the same thing twice (the Status
    # row even printed the raw uppercase MAC). The Board row stays, the Status
    # row goes - board copy keeps both. All JS writes to connStatus2 are
    # null-guarded, so nothing else changes.
    html = html.replace(
        """          <div class="cal-row-single">
            <label>Status:</label>
            <span id="connStatus2" class="mono conn-txt">Connected</span>
          </div>
""",
        "",
    )
    # Cloud device panel at the top of Settings (NOT above the status bar:
    # the header stays identical to the board-served dashboard).
    if 'id="cloudBar"' not in html:
        html = html.replace(
            '<section id="page-settings" class="page">',
            '<section id="page-settings" class="page">\n' + CLOUD_BAR,
            1,
        )
    # Minimalist settings for the cloud copy: fixed panel order, board-PIN
    # Admin panel dropped (badge lives in Cloud device), hint paragraphs out.
    html = _reorder_settings(html)
    # About hint: served from Firebase, not the board.
    html = html.replace(
        "served by the board itself, over its own WiFi",
        "cloud mirror &middot; pushed by the board every ~10&nbsp;s",
    )
    # Scripts: frontend script.js (rendering) + cloud.js (transport).
    # Drop the cache-busting query (hosting serves exact files).
    html = html.replace(
        '<script src="script.js?v=20261003b"></script>',
        '<script src="script.js"></script>\n<script src="cloud.js"></script>',
    )
    if '<script src="cloud.js"></script>' not in html:
        # Fallback for a future frontend that renames the query string.
        html = re.sub(
            r'<script src="script\.js[^"]*"></script>',
            '<script src="script.js"></script>\n<script src="cloud.js"></script>',
            html,
            count=1,
        )
    return html


def main() -> int:
    if not FRONT.is_dir():
        print("frontend/ missing", file=sys.stderr)
        return 1
    CLOUD.mkdir(exist_ok=True)

    # script.js: exact copy (rendering shared with the board dashboard).
    shutil.copy2(FRONT / "script.js", CLOUD / "script.js")

    # style.css: verbatim + cloud additions.
    css = (FRONT / "style.css").read_text(encoding="utf-8")
    if "Cloud-only additions" not in css:
        css = css.rstrip("\n") + "\n" + CLOUD_CSS
    (CLOUD / "style.css").write_text(css, encoding="utf-8")

    # index.html: patched copy.
    html = (FRONT / "index.html").read_text(encoding="utf-8")
    (CLOUD / "index.html").write_text(patch_index(html), encoding="utf-8")

    # PWA shell: same icons + manifest so install works from hosting too.
    shutil.copy2(FRONT / "manifest.json", CLOUD / "manifest.json")
    icons_src = FRONT / "icons"
    icons_dst = CLOUD / "icons"
    icons_dst.mkdir(exist_ok=True)
    for p in icons_src.glob("*.png"):
        shutil.copy2(p, icons_dst / p.name)

    # Cache-busting stamp, derived from CONTENT (not time) so the builder
    # stays a no-op when sources are unchanged: a returning browser must
    # never keep running a stale script.js after a deploy, and the service
    # worker only updates when sw.js bytes change.
    h = hashlib.sha1()
    for name in ("script.js", "cloud.js", "style.css"):
        h.update((CLOUD / name).read_bytes())
    stamp = h.hexdigest()[:8]

    html_path = CLOUD / "index.html"
    html = html_path.read_text(encoding="utf-8")
    html = re.sub(r'(src|href)="(script\.js|cloud\.js|style\.css)(\?v=[0-9a-z]+)?"',
                  lambda m: '%s="%s?v=%s"' % (m.group(1), m.group(2), stamp), html)
    html_path.write_text(html, encoding="utf-8")

    # Service worker: same logic, cloud cache name + cloud shell.
    # Both carry the content stamp so a deploy always busts every cache.
    sw = (FRONT / "sw.js").read_text(encoding="utf-8")
    sw = sw.replace("esp32-counter-v15", "esp32-counter-cloud-" + stamp)
    sw = re.sub(
        r"const SHELL = \[.*?\]",
        """const SHELL = [
  './index.html',
  './style.css?v=%s',
  './script.js?v=%s',
  './cloud.js?v=%s',
  './manifest.json',
  './icons/icon-192.png',
  './icons/icon-512.png'
]""" % (stamp, stamp, stamp),
        sw,
        flags=re.S,
    )
    (CLOUD / "sw.js").write_text(sw, encoding="utf-8")

    print("cloud-viewer rebuilt from frontend/ (+ cloud.js stays hand-written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

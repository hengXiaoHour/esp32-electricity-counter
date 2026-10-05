#!/usr/bin/env python3
"""Build cloud-viewer/ from frontend/ so both share the SAME UI.

The cloud dashboard is the local dashboard with a different transport:
  frontend/  : WebSocket to the board (location.host/ws), PIN auth.
  cloud-viewer/: Firebase RTDB (/devices/<MAC>/latest + /cmd + /ack),
               Google-admin auth, public read-only.

To keep the look identical without drifting, this script COPIES the
rendering files verbatim and applies only small cloud patches:
  index.html : + Firebase SDK, + cloud bar (device picker / Google sign-in /
               stale tag), PIN panel -> Google panel, script.js + cloud.js
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
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FRONT = ROOT / "frontend"
CLOUD = ROOT / "cloud-viewer"

FIREBASE_SDK = """<script src="https://www.gstatic.com/firebasejs/10.12.2/firebase-app-compat.js"></script>
<script src="https://www.gstatic.com/firebasejs/10.12.2/firebase-auth-compat.js"></script>
<script src="https://www.gstatic.com/firebasejs/10.12.2/firebase-database-compat.js"></script>
"""

CLOUD_BAR = """    <!-- Cloud bar: device picker + Google auth + freshness. The only
         chrome that differs from the board-served dashboard. -->
    <div id="cloudBar" class="panel-box cloud-bar">
      <div class="cloud-row">
        <span id="cloudDot" class="led" title="cloud freshness"></span>
        <select id="devicePicker" class="cloud-select" aria-label="device"></select>
        <button class="btn-sm" id="cloudReloadBtn" title="reload device list">Reload</button>
        <span id="staleTag" class="stale-tag hidden">STALE</span>
      </div>
      <div class="cloud-row">
        <span id="userEmail" class="mono dim"></span>
        <span id="adminBadge" class="role-badge role-admin hidden">ADMIN</span>
        <button class="btn-sm" id="authBtn">Sign in with Google</button>
      </div>
      <div id="cloudCmdStatus" class="mono dim cloud-status">No command sent yet.</div>
    </div>
"""

GOOGLE_PIN_PANEL = """        <div class="panel-box">
          <div class="panel-header"><h3>Admin</h3></div>
          <div class="pin-badge-row">
            <span id="roleBadge" class="role-badge role-guest">Viewer &mdash; read-only</span>
          </div>
          <div class="cal-row-single">
            <label>Google:</label>
            <div class="cal-field">
              <span class="mono" id="googleWho">not signed in</span>
            </div>
            <button class="btn-sm" id="authBtn2">Sign in</button>
          </div>
          <p id="guestHint" class="hint">Live readings are public. Changing limits, calibration, counters or the console needs the admin Google account &mdash; the database rules check every write, so hiding a button here is not what protects them.</p>
        </div>
"""

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
        """      Cloud mirror of the meter, pushed by the board every ~10&nbsp;s over home WiFi.
      Pick a device below &mdash; no board WiFi needed.""",
    )
    html = html.replace(
        "&#9654; Reconnect", "&#9654; Retry",
    )
    # Sidebar footer still says "Admin PIN" - cloud has no PIN, just Admin.
    html = html.replace(
        '<span class="nav-label">Admin PIN</span>',
        '<span class="nav-label">Admin</span>',
    )
    # Cloud bar at the top of <main>, before the status bar.
    if 'id="cloudBar"' not in html:
        html = html.replace(
            "    <!-- Status Bar -->\n    <header id=\"statusBar\">",
            CLOUD_BAR + "\n    <!-- Status Bar -->\n    <header id=\"statusBar\">",
        )
    # Admin PIN panel -> Google admin panel (keep roleBadge + guestHint ids:
    # script.js/cloud.js drive the badge from those).
    start = html.find('          <div class="panel-header"><h3>Admin PIN</h3></div>')
    if start >= 0:
        # The PIN panel-box runs from its opening div to the matching close:
        # find the opening <div class="panel-box"> just before the header.
        open_idx = html.rfind('<div class="panel-box">', 0, start)
        # Walk forward counting divs to the panel-box close.
        depth = 0
        i = open_idx
        end = -1
        for m in re.finditer(r"</?div\b", html[open_idx:]):
            tag = m.group(0)
            if tag == "<div":
                depth += 1
            else:
                depth -= 1
                if depth == 0:
                    end = open_idx + m.end()
                    # include the rest of the closing tag '>'
                    gt = html.find(">", end - 1)
                    end = gt + 1
                    break
        if end > 0:
            html = html[:open_idx] + GOOGLE_PIN_PANEL.rstrip("\n") + "\n" + html[end:]
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

    # Service worker: same logic, cloud cache name + cloud shell.
    sw = (FRONT / "sw.js").read_text(encoding="utf-8")
    sw = sw.replace("esp32-counter-v15", "esp32-counter-cloud-v1")
    sw = re.sub(
        r"const SHELL = \[.*?\]",
        """const SHELL = [
  './index.html',
  './style.css',
  './script.js',
  './cloud.js',
  './manifest.json',
  './icons/icon-192.png',
  './icons/icon-512.png'
]""",
        sw,
        flags=re.S,
    )
    (CLOUD / "sw.js").write_text(sw, encoding="utf-8")

    print("cloud-viewer rebuilt from frontend/ (+ cloud.js stays hand-written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

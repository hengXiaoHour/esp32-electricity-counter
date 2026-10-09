# Design — ESP32 Counter Dashboard

The visual language and the UI decisions worth remembering. **How the thing
works** lives in `doc/ARCHITECTURE.md`; this file is about how it *looks* and
why it looks that way.

## Visual Style

### Color Palette
| Token | Hex | Usage |
|-------|-----|-------|
| primary / trip | #E53935 | Launch button, active/focus borders, alerts |
| ok | #00C853 | Online LED dot, connected status |
| warn | #FF9800 | "Connecting..." status text |
| background | #121212 | Page background |
| surface (card) | #1E1E1E | Connect card, panel boxes |
| input | #1A1A1A | Inputs, dropdowns |
| border | #444444 | Card/input borders |
| text | #FFFFFF | Titles |
| text-2 | #888888 | Subtitle, captions, footer |
| text-3 | #666666 | Disabled/tertiary text |

### Typography
| Element | Font | Size | Weight |
|---------|------|------|--------|
| Title ("ESP32 Counter") | UI sans-serif | 1.15rem | 700, letterspaced 0.12em caps |
| Subtitle | UI sans-serif | 0.78rem | 400 |
| Inputs / selects | monospace | 0.9rem | 400 |
| Buttons | UI sans-serif | 0.78–0.82rem | 700, caps |

## The connect panel is gone (2026-10-02)

There used to be one: a mode dropdown (Cloud / Local / Demo), a full-width
Launch button, an IP input that appeared for Local, and a board picker for
Cloud. **All of it is gone.**

The page is served *by the board*, so `location.host` **is** the board — there is
no IP to type, no transport to choose, and no cloud to fall back to. `init()`
opens the socket immediately. What remains of the panel is not a connect form
but a single **"you are not on the board's WiFi"** notice with a Reconnect
button, shown only after the socket fails or closes.

Two knock-on simplifications: `isDemo` is a boolean rather than a three-value
mode string, and the Wi-Fi indicator has two states (`lv0` idle, `lv2` AP up)
instead of five, because an access point has no RSSI to report.

## Design Decisions

### Auto-zero queue status — "Please wait" (2026-08-13)
Multi-channel Auto-Zero clicks are queued on the board (firmware `requestAutoZero`),
so the dashboard must communicate busy state and queue progression.
- **Firmware adds to broadcast**: `azActive`, `azChannel`, `azProgress` (batch count
  / 32), `azQueue` (channels waiting). The WebSocket snapshot carries them via the
  shared `buildSystemJson`.
- **Status line** (`#azStatus`, coral `--trip`, mono 0.7rem) appears above the Channel
  Calibration list only while busy:
  `Please wait — calibrating Ch1 (18/32) · waiting: Ch3, Ch5`
- **Per-row chips** (`#azChip_n` + mirrored `#azHeadChip_n` on each collapse header so
  queued state is visible even in collapsed rows):
  - active: `◐ Calibrating…` — coral, pulsing (`pulse-border`), same visual language
    as the `.calibrating` input border
  - queued: `◷ Queued` — dim amber `--warn`
  - idle: hidden
- **Click feedback** (`autoZeroChannel`): clicking Auto-Zero on an already-active or
  queued channel shows "already in the auto-zero queue — please wait"; when another
  channel is calibrating the toast reports "Ch4 queued — please wait".
- **Auto-advance** is driven by the broadcast: when the active channel finishes, its
  Noise Floor box syncs to the committed median and the next queued channel becomes
  active automatically (chips/banner/box all move live).

### LPF box must not follow the firmware's capture bypass (2026-08-13)
The firmware forces the LPF alpha to 1.0 (no filtering) while capturing the auto-zero
samples, then restores the user value after commit. Naively syncing `lpfAlpha` from the
broadcast made the LPF box jump to `1.00` the moment Auto-Zero started and stay there
until calibration finished.
- **Firmware**: `computeAll()` bypasses filtering via `azLpfForced ? 1.0f : lpfAlpha[ch]`
  during capture — the saved user value in `lpfAlpha[ch]` is never touched, and the
  save/set/restore round-trip around auto-zero was removed.
- **Dashboard**: the broadcast sync is gated on `data.azActive !== true`, so the LPF box
  holds its last real value while the banner/chips communicate the busy state; it resumes
  syncing the instant the broadcast clears `azActive`.
- **Demo mock** broadcasts `lpfAlpha: [1.0 ×5]` (the firmware default) plus reset
  az fields, so demo mode matches a fresh board and cannot race the gate.

### Clock status is an indicator, not decoration (2026-10-02)
Settings shows the board's reported clock state (`#timeStatus`), including the
failure case in words: *"not set — monthly reset will not fire"*, in the trip
red.

This is unusual for a status readout because it is genuinely actionable: the
board has no NTP and borrows the browser's clock, and `rolloverIfNeeded()`
does nothing while the clock is unset. A silent clock means the monthly billing
reset quietly stops happening. Surfacing it turned an invisible failure into a
visible one.

### Trip notifications replace the ntfy push (2026-10-02)
There is no server to push to, so a trip raises a browser `Notification` while
the dashboard is open. Keyed on *which* channels are tripped rather than on
arrival, so a snapshot arriving 6× a second does not re-notify but a newly
tripped channel does. `tag` replaces the previous notification instead of
stacking them, and the first one of an episode uses `requireInteraction` so it
stays on screen while the buzzer is going.

### Auto-Zero is disabled while it runs (2026-10-09)
The Auto-Zero button used to stay live on the channel being calibrated. The board
refuses a second calibration for that channel (`requestAutoZero` returns false),
and the dashboard ignored the return value, so the tap produced no answer and the
toast still claimed the noise floor would update shortly. The button is now
`disabled` for the running channel and for anything already queued — the two cases
the firmware would refuse — while every idle channel stays enabled, because the
board runs a queue rather than refusing concurrent clicks. The colour moved from
an inline `style` to `.btn-sm.btn-az` for that: an inline colour beats any
`:disabled` rule, so a "disabled" button that still looked live was the bug.
The firmware now also reports the refusal instead of claiming success.

### The calibration status lives under its row, not in the field (2026-10-09)
The `◐ Calibrating…` chip sat inside the `.cal-field` cell beside the Noise Floor
input. On a 360 px phone the leftover width after the 85 px label, the input and
the 104 px Auto-Zero button is smaller than the text, so the chip painted itself
straight over the button it was describing — the status covered the control. It is
now a block under its own row, indented to the field column, and the Auto-Zero
button keeps a clean column at every width.

### The feedback box is a solid fill with white text (2026-10-09)
`#toast` was a dark card with a 1 px green edge. It is now a filled box with
fully white text. The fill is deliberately deeper than the `--ok` accent
(`#3ec972`), because white on that accent is only ~2.1:1 while white on this
pair is ~5:1 — "fully white text" has to stay readable, not just literal.
`showToast(msg, ms, kind)` gives it three fills (`ok` green, `warn` amber,
`err` red) and `showError()` routes every rejected value through the red one, so
"LPF Alpha must be 0.01-1" can never be painted in the colour of a success.

### Install is a hosted-site feature, not a board feature (2026-10-09)
The Install button used to ship in the board's own page. It could never work
there: the board serves its dashboard over plain http on a LAN IP, which is not
a secure context, so no browser registers the service worker and none fires
`beforeinstallprompt`. The button was a dead control on the one origin where
"Install" is never offered — clicking it could only produce the manual-steps
shrug.

So the install UI moved entirely into the cloud build, where the origin is
https and the prompt genuinely fires:

- `frontend/` (the board's copy) has **no** install markup, CSS or JS. Not a
  hidden button — nothing, which also keeps ~1 KB out of a firmware image that
  runs at 75% flash.
- `cloud-viewer/install.js` is hand-written next to `cloud.js`, and
  `build_cloud_viewer.py` injects its markup (connect-panel row + header button
  + manual-steps hint), its `<script>` tag and its CSS into the hosted page.
- `install.js` is in the cloud service worker's precache shell and in the
  content-hash stamp, so a change to it actually reaches a returning browser.

The board copy keeps its `<link rel="manifest">`: with no service worker Chrome
simply never offers "Install", which is exactly the wanted behaviour. Five
`check_docs.py` claims, one `e2e_aponly.js` assertion and five
`e2e_cloud_settings.js` assertions pin the split, all mutation-tested.

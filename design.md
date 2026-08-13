# Design — ESP32 Counter Connect Panel & Dashboard

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

## Connect Panel (2026-08-11)
All three connection modes (Cloud / Local / Demo) live in ONE dropdown
(`#connMode` — options in that order, **Cloud is the default** when no saved
mode), with a single full-width coral **Launch** button
(`#handleConnect`) that routes to `connectLocal` / `connectCloud` /
`startDemoMode` based on the selection. Fields revealed contextually:
- **Local** → IP input row shown
- **Cloud** → device picker (`#devicePicker`) shown
- **Demo** → no extra fields

Selected mode persisted in `localStorage('esp32monitor_connmode')` and field
visibility driven by `updateConnectFields()`. Status banner (`#connectStatus`)
still reports Ready / Connecting (amber) / Connected (green) / error (red).
Layout: card stacked rows — dropdown, conditional field row, Launch, status,
footer explanation, Install row.

### Wireframe
```
┌─────────────────────────────┐
│   ● ESP32 COUNTER           │
│   ESP32-S3 · 6-Channel ...   │
│  ┌───────────────────────┐  │
│  │ Local — WebSocket    ▼ │  │  #connMode dropdown
│  └───────────────────────┘  │
│  ┌───────────────────────┐  │  #ipRow (local only)
│  │ 192.168.1.x           │  │
│  └───────────────────────┘  │
│  ┌───────────────────────┐  │  #cloudRow (cloud only)
│  │ esp-858428           ▼ │  │
│  └───────────────────────┘  │
│  ┌───────────────────────┐  │
│  │      ▶ LAUNCH         │  │  #handleConnect
│  └───────────────────────┘  │
│  ┌───────────────────────┐  │
│  │ Ready to connect      │  │  #connectStatus
│  └───────────────────────┘  │
│  ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─  │
│ Cloud = Firebase RTDB · ...│
│   [ Install App ]          │
└─────────────────────────────┘
```

## Design Decisions

### Auto-zero queue status — "Please wait" (2026-08-13)
Multi-channel Auto-Zero clicks are queued on the board (firmware `requestAutoZero`),
so the dashboard must communicate busy state and queue progression.
- **Firmware adds to broadcast**: `azActive`, `azChannel`, `azProgress` (batch count
  / 32), `azQueue` (channels waiting). WebSocket and Firebase /latest both carry them
  via the shared `buildSystemJson`.
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
  syncing the instant the broadcast clears `azActive`. Test 9 pins this behavior.
- **Demo mock** corrected: it broadcast `lpfAlpha: [1,1,1,1,1,1]` (and no `azActive`),
  so demo mode showed `1.00` LPF boxes and could race the gate — now `0.2` + reset az fields.

### Single mode dropdown + Launch button (2026-08-11)
- **Context**: Connect panel had three separate buttons (Local primary,
  Cloud/Demo outline) plus an IP input and a device picker — cluttered, and
  the "active mode" was visually ambiguous.
- **Options**: A) keep three buttons; B) radio-style segmented control;
  C) mode dropdown + one Launch button.
- **Choice**: C — one dropdown selects the transport, one primary button
  launches it. Fields (IP vs device picker) toggle with the selection, so the
  card always shows only what's relevant.
- **Tradeoffs**: one extra click to switch modes, but clearer affordance that a
  connection is a single begin/end action; matches the existing amber
  "Connecting to Firebase..." status flow (no per-mode button state to track).
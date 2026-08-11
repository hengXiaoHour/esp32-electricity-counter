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
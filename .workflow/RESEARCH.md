# Research: UX/Interface Improvements

## Current State

Three files in `data/` — externally-hosted WebSocket dashboard, dark/red theme, working features.

### Current HTML structure (index.html)
- Single-tier header: `#headerLeft` (title + badge) + `#sysInfo` (voltage, wifi, clock, LED) in one flex row
- Connection bar below header
- OTA panel (hidden by default)
- Channel cards container
- Event log panel
- Calibration settings panel

### Current CSS (style.css)
- Dark theme: `#0c0c10` bg, `#15151a` cards, `#e63946` red accent
- Cards: 1px border, subtle hover lift, border-color on status classes
- `#channels` grid: `repeat(auto-fill, minmax(280px, 1fr))`
- Responsive breakpoints at 480px and 360px
- `.label` uses `#777` at `.65rem` — low contrast

### Current JS (script.js)
- `updateDashboard()` renders cards in payload order (no sort)
- 6 equal-weight stats per card (Current, Power, Apparent, PF, Energy, Relay)
- 3 inputs + 2 buttons always visible per card (limit, power limit, name, Save, Reset)
- `resetRelay()` sends immediately on click — no confirmation
- `setLimit()` sends immediately — no feedback
- Event log: simple list, last 10 events, no filters
- No `aria-live` attributes

## Requirements (from prompt)

1. **Two-tier header** — brand/status top, secondary info bottom strip
2. **Problem channels louder** — sort tripped/warning first, background tint, thicker border, status icons (✓/⚠/✗)
3. **Reduced card density** — promote Current/Power/Energy, demote Apparent/PF, edit mode toggle
4. **Edit mode** — read-only display by default, Edit button reveals inputs + Save/Cancel per card
5. **Reset confirmation** — inline "Confirm Reset?" with 3s timeout before sending
6. **Save feedback** — pending state, success flash, error timeout (~5s)
7. **Event log filters** — All/Warnings/Trips filter, per-channel optional
8. **Accessibility** — aria-live, icons alongside color, higher contrast labels

## Risks
- Sort changes the card index order — `resetRelay(i)` and `setLimit(i)` use the sorted array index; must ensure the index sent to ESP32 matches the original payload index, not the display index
- Edit mode adds complexity — must track per-card edit state
- Reset confirmation timer + pending feedback state machine could get complex
- Must not break: hasRelay, per-channel cal, blink states, OTA panel, connect flow

## Approach
- Sort the display copy of channels, but keep original index mapping for commands
- Use `data-orig-index` attribute on cards to map display position → payload index
- Store edit state in a `Set` of card indices
- Use `aria-live="polite"` on channels container and event list
- Use Unicode glyphs (✓ ⚠ ✕) for status icons

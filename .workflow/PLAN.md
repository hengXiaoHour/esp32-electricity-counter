# Plan: UX/Interface Improvements

## Files to Modify
- `data/index.html` — header restructure, event filter controls, aria-live attributes
- `data/style.css` — all new styles for header tiers, edit mode, confirm flow, status icons, a11y
- `data/script.js` — sort, edit mode, confirm, feedback, filters, icons, index mapping

## Implementation Steps

### Step 1: Two-tier header (HTML + CSS)
- Split `#header` → `#headerTop` (title + badge + LED status dot) + `#headerInfo` (voltage, wifi, clock)
- LED status dot gets a text label alongside for color-blind accessibility
- 320px test: no mid-word wraps, no overlap

### Step 2: Sort + status icons (JS + CSS)
- Sort channels: tripped first, warning, ok, off. Map display idx → orig idx via `data-orig-index`
- Prepend Unicode glyph to status badge: ✓ OK, ⚠ WARNING, ✕ TRIPPED, — DISABLED
- CSS: `.ch-status .icon-ok`, `.icon-warn`, `.icon-trip` for glyph styling
- Thicker borders: 2px for warning, 3px for tripped
- Background tint: `.card.warning` gets subtle amber bg, `.card.tripped` gets subtle red bg
- Tag `aria-live="polite"` on channels container

### Step 3: Card density + edit mode (JS + CSS + HTML)
- Stats grid: primary row = Current, Power, Energy (promoted); secondary row = Apparent, PF (demoted, smaller); Relay inline
- Read-only default: show "Limit: X A / Y W" text, name as plain text, Edit icon-button
- Edit mode toggle: per-card `editingCards` Set, Edit button reveals inputs + Save/Cancel
- Cancel reverts input values to last-saved state without sending anything
- `data-orig-index` attribute on `.card` for index mapping

### Step 4: Reset confirmation (JS + CSS)
- `resetRelay()` → show inline "Confirm Reset?" on the button for 3s
- Click again within 3s: send command + clear timer
- Click elsewhere or wait 3s: revert button text
- No native `confirm()` dialog

### Step 5: Save feedback (JS + CSS)
- On setLimit/sendCal click: button shows "Saving..." + disabled
- On next updateDashboard that confirms value match: green success flash (500ms)
- Timeout 5s: show "Failed" + error styling, re-enable button
- Same pattern for Reset after confirmation

### Step 6: Event log filters (JS + CSS + HTML)
- Filter bar above event list: All | Warnings | Trips
- Click filter → re-render events list filtered by `ev.s`
- Note comment: display only shows last 10 from payload

### Step 7: Accessibility (CSS)
- `.label` color: `#777` → `#aaa` for WCAG AA contrast
- LED status: add visually-hidden text label alongside dot
- Connection badge: ensure `aria-label`

### Step 8: Verify
- Inline all files into Playwright setContent
- Run comprehensive test: 6 cards, statuses, edit toggle, reset confirm, filters, header tiers
- Verify no regressions: hasRelay, per-channel cal, blink, OTA, connect flow

## Test Strategy
- Playwright: setContent with inlined files → mock data → verify all features
- Check at 320px viewport via `page.setViewportSize`

## Rollback
- All 3 files in `data/` — restore from git

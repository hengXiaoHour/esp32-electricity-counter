# Plan: Fix Dashboard Bugs + Red Dark Theme

## Files to Modify
- `data/index.html` — clean skeleton, link to external CSS/JS, add OTA panel, per-channel cal rows
- `data/style.css` — complete dark/red theme redesign with blink animations
- `data/script.js` — rewritten with hasRelay, per-channel cal, blink toggling, OTA panel

## JSON Payload Changes (documented in script.js header comment)

### Added fields
| Field | Location | Type | Description |
|-------|----------|------|-------------|
| `hasRelay` | per-channel `ch[i]` | bool | true for relay-controlled channels (1-4), false for monitor-only (5-6) |
| `currentCalibration` | root | number[] | Array of 6 values, one per channel |
| `otaProgress` | root | number (optional) | OTA update progress 0-100 |
| `firmwareVersion` | root | string (optional) | Current firmware version (e.g. "1.0.0") |

### Changed fields
| Field | Before | After |
|-------|--------|-------|
| `currentCalibration` | single number | array of 6 numbers |
| `voltageCalibration` | unchanged | unchanged (stays global) |

### Removed fields
None.

### New commands
| Command | Payload | Description |
|---------|---------|-------------|
| `set_current_cal` | `{cmd, ch: int, val: float}` | Set per-channel current calibration |

### Changed commands
| Command | Before | After |
|---------|--------|-------|
| `set_voltage_cal` | `{cmd, val}` | unchanged |
| `set_current_cal` | `{cmd, val}` (global) | `{cmd, ch, val}` (per-channel) |

## Implementation Steps

### Step 1: Rewrite `data/index.html`
- Remove all inline `<style>` and `<script>` blocks
- Add `<link rel="stylesheet" href="style.css">`
- Add `<script src="script.js" defer></script>`
- Add OTA panel div (hidden by default, between header and channels)
- Add `#currentCalRows` container in settings panel for per-channel calibration
- Changed: `sendCal('set_voltage_cal','voltCal')` → `sendVoltageCal()`
- Remove old global `currCal` input row

### Step 2: Rewrite `data/style.css`
- Base: `#0c0c10` body bg, `#15151a` card bg, `#2a2a32` border
- Brand red: `#e63946` for header, buttons, hover borders
- OK status: `#2ecc71` green (semantic, distinct from red)
- Warning: `#f39c12` amber
- Tripped: `#ff1744` bright red
- Typography: system sans-serif for UI labels, `mono` class for numeric readings
- Cards: 1px border, `border-radius: 10px`, `box-shadow`, border color changes on status
- `@keyframes blink-yellow` / `@keyframes blink-red` — opacity + box-shadow pulse
- `.blink-yellow`, `.blink-red`, `.blink-blue` classes
- `.monitor-only` muted gray tag
- OTA panel styles (gradient bar, progress fill)
- Button styles: primary (brand red bg), reset (bright red outline), secondary (muted)
- Hover states: slight lift (`translateY(-1px)`), border glow
- Smooth transitions: 150-200ms
- Responsive: keep `auto-fill, minmax(280px, 1fr)`

### Step 3: Rewrite `data/script.js`
- Preserve all existing functionality: `connectWS()`, `updateDashboard()`, `setLimit()`, `resetRelay()`, `formatUptime()`, `formatTime()`, `escHtml()`
- **hasRelay**: check `ch.hasRelay` — if false, hide relay indicator, show "Monitoring Only", no Reset button; if true, show relay ON/OFF + Reset button always
- **Blink**: set `ledEl.className` to `'blink-yellow'`/`'blink-red'`/`'blink-blue'` based on state; apply same class to channel status badge
- **Per-channel cal**: `initCalibrationRows(count)` generates row HTML once; `sendCurrentCal(ch)` sends `{cmd:'set_current_cal', ch, val}`
- **OTA panel**: show/hide `#otaPanel`, update progress bar width + percentage text
- **Voltage cal**: `sendVoltageCal()` sends `{cmd:'set_voltage_cal', val}`
- Add `mono` class to `<span class="value mono">` for readings
- Each `data.ch[i]` gets `hasRelay` field; Reset button appears on all relay channels (not just tripped)

## Verification
- Open `index.html` in browser — should render with dark/red theme
- Check that cards render correctly for all states
- Verify calibration section shows 6 current inputs + 1 voltage input
- Verify OTA panel renders with progress bar
- Verify blink animations work on LED dot and status badges
- Manual: check all button onclick handlers reference existing functions
- Mobile: test at 320px width

## Rollback
- All 3 files are in `data/` — restore from git if available, or keep originals as backup

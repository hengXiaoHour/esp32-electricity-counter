# Research: Voltage Bug (240V → 147V on any cal op)

## Status: ROOT CAUSE ISOLATED (2026-08-12, by math reproduction)

## Symptom
Voltage reading drops from ~240V to ~147V when ANY calibration operation runs.
Not just `reset_ch_cal` — user says any cal op. Debug logging added last session
logs **`powerCalc->voltageCal`** before/after — and it never changes,
so serial confrmed nothing.

## Root Cause
`src/core/power_calculator.cpp:33`:
```cpp
float vAlpha = lpfAlpha[0];        // voltage shares CHANNEL 0's LPF alpha
```

The voltage channel is low-pass filtered with the **same per-channel, user-tunable
LPF alpha as channel 0** (introduced 2026-08-06 for "matched filter keeps real
power phase-accurate"). An EMA low-pass on a 50 Hz AC waveform attenuates its
RMS amplitude. Simulated exact firmware math (n=1000 samples, 40us @ 25kHz):

| lpfAlpha[0] | gain @50Hz | 230V becomes |
|---|---|---|
| 1.0 (default) | 1.000 | 230V |
| 0.05 | 0.972 | 223V |
| 0.02 | 0.854 | 196V |
| 0.012 | 0.705 | 162V |
| **0.010 (UI min)** | **0.640** | **147V** |

**240V→147V is exactly the alpha=0.01 low-pass behavior.** Once the user sets
channel 0 LPF to 0.01 (or reset_ch_cal / autozero interplay leaves any value
below ~1.0), the shared voltage magnitude is permanently attenuated until that
alpha is restored. Because the *displayed* voltage is `data.v` (firmware
`voltageRMS`, power_calculator.cpp:11), no frontend scaling exists — pure firmware.

## Why "any cal op" appears to trigger it
- The bug doesn't fire DURING an op — it fires the instant `lpfAlpha[0]` < 1.0.
- `set_lpf ch:0` writes lpfAlpha[0] directly. Other ops (set_current_cal,
  reset_ch_cal, auto-zero, reset_nvs_defaults) don't touch it, so if the board
  already has lpfAlpha[0]=0.01 from a previous session/NVS, voltage shows 147
  after every command — looks like "every op breaks it."
- NVS persisted `lpfAlpha[0]` (loadLpfAlpha in .ino:281, saveNvs per set_lpf)
  survives reboot, so it looks permanent.

## Fix options
- **A (recommended): decouple voltage from ch0 LPF.** Give voltage its own
  LPF alpha (or force 1.0 = unfiltered). Voltage is a single reference signal;
  per-channel current LPF should never alter it. Concerns: power cross-correlation
  "matched filter" comment — but V is one shared signal, filtering it with ch0's
  alpha is a false match for ch1-5 anyway.
- **B: separate `voltageLpfAlpha` NVS key + UI.** Most correct, more surface.
- C: clamp lpfAlpha to e.g. >=0.5 — hides the bug, wrong fix.

Risks: NVS migration for new key (B); existing board state with lpfAlpha[0]=0.01
must be reset by the fix path (reset_ch_cal or reset_nvs_defaults still work).

## Verification needed (hardware)
1. `set_lpf ch:0 val:0.01` from dashboard → voltage drops 240→147. Confirms.
2. `reset_ch_cal ch:0` (sets lpfAlpha[0]=1.0) → voltage returns to 240. Confirms.
3. After fix: `set_lpf ch:0 val:0.01` must NOT change voltage.

---

# Research: Per-Channel Calibration Reset Button

## Current State

The System Calibration panel (Settings page, `admin-only`) has one **global**
"Reset NVS to Defaults" button (`handleResetNvs` → `reset_nvs_defaults`). It
wipes ALL 6 channels' calibration (currentCal, noiseFloor, lpfAlpha) plus
voltage cal + rmsSamples in one shot. After hitting it, the user must manually
re-enter every other sensor's calibration — annoying.

There is already a per-channel "Reset to Default" in the **Edit modal**
(`resetModalToDefaults` → `reset_ch_to_default`), but that command ONLY resets
the channel **name + monthly kWh limit**, NOT its calibration values
(`command_processor.cpp:218-236`). So the calibration panel lacks any
per-channel reset.

## Requirements

1. In the System Calibration panel, each channel's collapsible row should get a
   small reset button that resets ONLY that channel's NVS calibration back to
   defaults (currentCal=100, noiseFloor=0, lpfAlpha=1).
2. Affected sensors' live values (`powerCalc`) + `sysData.currentCalibration[]`
   must update immediately (not just on reboot), mirroring `reset_nvs_defaults`.
3. Other channels and voltage/rms settings must be untouched.

## Approach Options

- **A (chosen): new `reset_ch_cal` command** — add a dedicated per-channel
  calibration reset in `command_processor.cpp`, mirroring the loop body of
  `reset_nvs_defaults` but for a single channel. Frontend adds a small
  "Reset Cal" button in each channel's calibration row.
  - Pros: surgical, no surprise to edit-modal behavior, reuses existing
    `saveChannelCurrentCal`/`saveNoiseFloor`/`saveLpfAlpha`/`setLpfAlpha` etc.
  - Cons: new command string; needs firmware re-upload (board must be flashed).
- **B: extend `reset_ch_to_default`** to also reset calibration — would change
  the Edit modal's "Reset to Default" semantics too (name+limit+cal in one go).
  - Pros: one command fewer.
  - Cons: changes existing behavior of a separate UI path; mixes concerns.
- **C: frontend-only** (reset all NVS then... no) — impossible; there's no
  per-channel firmware reset.

## Risks

- Command ordering: `reset_ch_cal` must be matched before the generic
  `reset_ch_to_default` branch and not collide with `reset_counter`.
- `ch` range validation: only accept 0..NUM_CHANNELS-1.
- Needs `DEFAULT_CURRENT_CALIBRATION`, `setNoiseFloor`, `setLpfAlpha` —
  all already exposed (`config.h:53`, `power_calculator.h`).
- Firmware changes require re-uploading to the board; frontend change alone
  won't make the button functional.

## Open Questions

- None blocking. UX: single-click (no 2-step confirm) for a cal reset — it's
  easily re-tunable, unlike the full NVS wipe which keeps its confirm.

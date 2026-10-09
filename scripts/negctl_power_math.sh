#!/usr/bin/env bash
# Negative controls for scripts/test_power_math.c.
#
# A test that has only ever printed PASS is untested. This reintroduces each
# defect into a SCRATCH COPY of the firmware (never the working tree), rebuilds
# the host test's expectations against it, and requires the suite to FAIL.
#
#   N1  restore the one-pass variance E[X^2] - E[X]^2
#   N2  restore the `voltageRMS * noiseFloor` power subtraction
#
# Both write to $WORK (default /tmp/opencode/pc-negctl), never to the repo.
set -u

WORK="${WORK:-/tmp/opencode/pc-negctl}"
SRC="$(cd "$(dirname "$0")" && pwd)/../src/core/power_calculator.cpp"
mkdir -p "$WORK"
cp "$SRC" "$WORK/pristine.cpp"

restore() { cp "$WORK/pristine.cpp" "$WORK/mutant.cpp"; }
sha_before=$(sha256sum "$SRC" | cut -d' ' -f1)

run_case() {
  local name="$1" desc="$2"
  echo "--- $name: $desc"
  # The test hardcodes the maths, so a mutant must edit BOTH the reference
  # copy and the test's transcription of it. Editing only the test would prove
  # nothing about the firmware; editing only the firmware proves nothing,
  # because the host test never reads the firmware.
  if cc -O2 -o "$WORK/t" "$WORK/test.c" -lm 2>"$WORK/cc.log"; then
    if "$WORK/t" >"$WORK/out.log" 2>&1; then
      echo "    *** SURVIVED *** the suite still passed -- the test is blind here"
      FAILED=1
    else
      echo "    CAUGHT (exit $?):"
      sed -n 's/^/      /p' "$WORK/out.log" | grep -m4 'FAIL'
    fi
  else
    echo "    *** BUILD FAILED (broken mutant, not a blind assertion) ***"
    sed -n 's/^/      /p' "$WORK/cc.log" | head -5
    FAILED=1
  fi
  restore
}

cp "$(dirname "$0")/test_power_math.c" "$WORK/test.c"
restore
FAILED=0

echo
echo "=============================================================="
echo "  N1: revert the current variance to one-pass"
echo "=============================================================="
python3 - "$WORK/test.c" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p).read()
old = """static float rmsTwoPass(const float *x) {          /* what the board does NOW */
  float sum = 0.0f;
  for (int i = 0; i < N; i++) sum += x[i];
  float mean = sum / (float)N;
  float acc = 0.0f;
  for (int i = 0; i < N; i++) { float d = x[i] - mean; acc += d * d; }
  return sqrtf(acc / (float)N) / (float)ADC_MAX * (float)VREF * (float)CURRENT_CAL;
}"""
new = """static float rmsTwoPass(const float *x) {          /* MUTANT: one-pass again */
  float isum = 0.0f, isq = 0.0f;
  for (int i = 0; i < N; i++) { isum += x[i]; isq += x[i] * x[i]; }
  float mean = isum / (float)N, meansq = isq / (float)N;
  return sqrtf(meansq - mean * mean) / (float)ADC_MAX * (float)VREF * (float)CURRENT_CAL;
}"""
assert old in s, "N1 anchor not found"
open(p,'w').write(s.replace(old, new))
print("    mutant written")
PY
run_case N1 "one-pass E[X^2]-E[X]^2"

echo
echo "=============================================================="
echo "  N2: restore the noise-floor power subtraction"
echo "=============================================================="
python3 - "$WORK/test.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = """  return (double)fabsf(pMean * adcToVolt * adcToVolt * (float)VOLTAGE_CAL * (float)CURRENT_CAL);
}"""
new = """  float pW = pMean * adcToVolt * adcToVolt * (float)VOLTAGE_CAL * (float)CURRENT_CAL;
  return (double)fabsf(pW);   /* MUTANT: caller re-adds the noise-power term */
}"""
assert old in s, "N2 anchor not found"
open(p,'w').write(s.replace(old, new))
print("    mutant written")
PY
# N2 also needs powerNow to carry the subtraction so the suite sees it.
python3 - "$WORK/test.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "  double wNew = powerNow(volt, cur);"
new = "  double wNew = powerNow(volt, cur) - 230.0 * floorA;  /* MUTANT */\n  if (wNew < 0.0) wNew = 0.0;"
assert old in s, "N2b anchor not found"
open(p,'w').write(s.replace(old, new))
print("    mutant wiring written")
PY
run_case N2 "voltageRMS * noiseFloor subtracted from active power"

echo
echo "=============================================================="
echo "  integrity"
echo "=============================================================="
sha_after=$(sha256sum "$SRC" | cut -d' ' -f1)
if [ "$sha_before" = "$sha_after" ]; then
  echo "  OK   src/core/power_calculator.cpp unchanged by the controls"
else
  echo "  *** THE WORKING TREE WAS MODIFIED ***"
  FAILED=1
fi

echo
if [ "$FAILED" -eq 0 ]; then
  echo "ALL MUTANTS CAUGHT"
else
  echo "SOME MUTANTS SURVIVED OR BROKE"
fi
exit $FAILED

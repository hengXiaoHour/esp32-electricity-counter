/*
 * Host regression test for the current/power trim maths in
 * src/core/power_calculator.cpp::computeAll().
 *
 * Why this exists (2026-10-09): the board trims the sensor's DC offset by
 * taking a variance, which is correct and exact. Two defects sat on top of it:
 *
 *   1. The variance was one-pass: E[X^2] - E[X]^2. At the ~2048-count
 *      mid-supply bias E[X^2] ~ 4.19e6, where one float32 ULP is 0.5 counts^2,
 *      while a 0.05 A load has a variance of ~0.15 counts^2. Small signals came
 *      out as rounding noise. Fixed by splitting into a mean pass and a
 *      variance pass.
 *   2. Active power subtracted `voltageRMS * noiseFloor` even though pWatts is
 *      a covariance of two mean-removed signals and therefore already excludes
 *      the DC offset. On a low-PF load the two trims disagreed badly enough to
 *      clamp real power to 0 W while the channel still showed current flowing,
 *      which then tripped the AUTO_RECOVER_PF auto-recovery. Fixed by dropping
 *      the term.
 *   3. No-load PF was computed down to S > 0.001 VA, so an empty socket got a
 *      "valid" PF that is really a ratio of two noise numbers, flickering
 *      across the 0.2 auto-recover line and re-ringing a latched trip one 80 ms
 *      cycle at a time, while fabs() rectified noise crept ~1-2 W into kWh.
 *      Fixed with a no-load deadband (P=0/S=0/PF=0, no integration below
 *      0.05 A or 2 W), a 5 VA PF floor, and ring persistence in the trip
 *      machine (PF must hold above 0.2 for 19 straight cycles, ~1.5 s,
 *      before a silenced trip re-rings; sections 6-8 below).
 *
 * IMPORTANT: the accumulators below are `float`, not `double`, on purpose. The
 * whole of defect 1 is a float32 precision problem, so a host test written in
 * double cannot see it -- it passes against the broken formula. Compiled with
 * -O0/-O2 on any IEEE-754 host this reproduces ESP32 arithmetic exactly.
 *
 * Build:  cc -O2 -o /tmp/pctest scripts/test_power_math.c -lm && /tmp/pctest
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
/* M_SQRT2 is a POSIX/GNU extension, not standard C11 - -std=c11 hides it. */
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

#define N            1000
#define ADC_MAX      4095.0
#define VREF         3.3
#define CURRENT_CAL  100.0          /* src/config.h DEFAULT_CURRENT_CALIBRATION */
#define VOLTAGE_CAL  260.0          /* src/config.h DEFAULT_VOLTAGE_CALIBRATION */
#define BIAS         2048.0

/* One ADC count, in amps, at currentCal=100. This is the resolution floor:
 * 0.01 A is an eighth of a count and cannot be measured at this gain. */
static const double A_PER_COUNT = (1.0 / ADC_MAX) * VREF * CURRENT_CAL;

static int failures = 0;
static int checks = 0;

static void check(int cond, const char *what) {
  checks++;
  if (!cond) { failures++; printf("  FAIL %s\n", what); }
}

static void near(double got, double want, double tol, const char *what) {
  checks++;
  if (fabs(got - want) > tol) {
    failures++;
    printf("  FAIL %s: got %.5f, want %.5f (tol %.5f)\n", what, got, want, tol);
  }
}

/* Deterministic uniform noise so fixtures are identical on every host. */
static uint32_t rngState = 0x12345678u;
static double noise(double sigma) {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  double u = ((double)(rngState & 0xFFFFFFu) / (double)0xFFFFFFu) - 0.5;
  return u * 2.0 * sigma * 1.732;
}

static float clampCounts(double v) {
  if (v < 0.0) return 0.0f;
  if (v > ADC_MAX) return (float)ADC_MAX;
  return (float)v;
}

/* Current channel. rmsA is a TRUE RMS value (so peak = rms * sqrt(2));
 * phaseDeg shifts current relative to voltage, which is what sets the PF. */
static void buildCurrent(float *buf, double dcCounts, double rmsA,
                         double rippleCounts, double phaseDeg) {
  double peak = (rmsA / A_PER_COUNT) * M_SQRT2;
  double ph = phaseDeg * M_PI / 180.0;
  for (int i = 0; i < N; i++) {
    double t = 2.0 * M_PI * i / N;
    double v = BIAS + dcCounts + peak * sin(t - ph) + noise(rippleCounts);
    buf[i] = clampCounts(v);
  }
}

static void buildVoltage(float *buf, double rmsVolts) {
  double adcRms = (rmsVolts / VOLTAGE_CAL) / VREF * ADC_MAX;
  double peak = adcRms * M_SQRT2;
  for (int i = 0; i < N; i++) {
    double t = 2.0 * M_PI * i / N;
    buf[i] = clampCounts(BIAS + peak * sin(t) + noise(1.0));
  }
}

/* ---- the variance forms, transcribed from computeAll() ------------------ */

static float rmsTwoPass(const float *x) {          /* what the board does NOW */
  float sum = 0.0f;
  for (int i = 0; i < N; i++) sum += x[i];
  float mean = sum / (float)N;
  float acc = 0.0f;
  for (int i = 0; i < N; i++) { float d = x[i] - mean; acc += d * d; }
  return sqrtf(acc / (float)N) / (float)ADC_MAX * (float)VREF * (float)CURRENT_CAL;
}

static float rmsOnePass(const float *x) {          /* the OLD, broken form */
  float isum = 0.0f, isq = 0.0f;
  for (int i = 0; i < N; i++) { isum += x[i]; isq += x[i] * x[i]; }
  float mean = isum / (float)N, meansq = isq / (float)N;
  return sqrtf(meansq - mean * mean) / (float)ADC_MAX * (float)VREF * (float)CURRENT_CAL;
}

/* Quadratic (RSS) noise-floor subtraction, as computeAll() :96-99. */
static double trimToNoiseFloor(double raw, double floorA) {
  return (raw * raw > floorA * floorA) ? sqrt(raw * raw - floorA * floorA) : 0.0;
}

/* Active power, current code: covariance only, no noise-power subtraction. */
static double powerNow(const float *v, const float *i) {
  float vMean = 0.0f, iMean = 0.0f;
  for (int k = 0; k < N; k++) { vMean += v[k]; iMean += i[k]; }
  vMean /= (float)N; iMean /= (float)N;
  float pSum = 0.0f;
  for (int k = 0; k < N; k++) pSum += (v[k] - vMean) * (i[k] - iMean);
  float pMean = pSum / (float)N;
  float adcToVolt = (float)VREF / (float)ADC_MAX;
  return (double)fabsf(pMean * adcToVolt * adcToVolt * (float)VOLTAGE_CAL * (float)CURRENT_CAL);
}

/* Active power, OLD code: the bogus `voltageRMS * noiseFloor` term. */
static double powerOld(const float *v, const float *i, double floorA) {
  float vMean = 0.0f;
  for (int k = 0; k < N; k++) vMean += v[k];
  vMean /= (float)N;
  float acc = 0.0f;
  for (int k = 0; k < N; k++) { float d = v[k] - vMean; acc += d * d; }
  float vRms = sqrtf(acc / (float)N) / (float)ADC_MAX * (float)VREF * (float)VOLTAGE_CAL;
  double out = powerNow(v, i) - (double)vRms * floorA;
  return out < 0.0 ? 0.0 : out;
}

/* ---- the no-load deadband + PF floor, transcribed from computeAll() ------ */
#define NO_LOAD_I  0.05          /* src/config.h NO_LOAD_CURRENT_A */
#define NO_LOAD_P  2.0           /* src/config.h NO_LOAD_POWER_W */
#define PF_MIN_VA  5.0           /* src/config.h PF_MIN_VA */
#define RECOVER_PF 0.2            /* src/config.h AUTO_RECOVER_PF */
#define RING_STABLE_N 19          /* src/config.h PF_RING_STABLE_CYCLES (~1.5 s) */

/* Mirrors the firmware exactly: deadband blanks P/S/PF together, otherwise
 * S is always reported and only PF is floored. */
static double gateLoad(double filtI, double rawP, double vRms,
                       double *sOut, double *pfOut) {
  if (filtI < NO_LOAD_I || rawP < NO_LOAD_P) {
    *sOut = 0.0; *pfOut = 0.0; return 0.0;
  }
  double s = vRms * filtI;
  *sOut = s;
  if (s > PF_MIN_VA) {
    double pf = rawP / s;
    if (pf > 1.0) pf = 1.0;
    *pfOut = pf;
  } else {
    *pfOut = 0.0;
  }
  return rawP;
}

/* ---- the recover + ring-persistence logic, transcribed from
 * LimitManager::checkLimits() ----
 * Latch/notify/event-log omitted: only the OK-vs-TRIPPED decision is modelled,
 * which is what drives the buzzer. Silence is instant below the line; ringing
 * from a recovery needs RING_STABLE_N consecutive cycles above it. */
static int hTripped, hRecovered, hStatus, hStable;   /* 0 = OK, 1 = TRIPPED */

static void hystReset(void) { hTripped = hRecovered = hStatus = hStable = 0; }

static void hystStep(double energy, double limit, double pf) {
  if (limit > 0.0 && energy >= limit) {
    hTripped = 1;
    if (!hRecovered) {
      if (pf < RECOVER_PF) { hRecovered = 1; hStable = 0; hStatus = 0; }
      else hStatus = 1;
    } else {
      if (pf > RECOVER_PF) { if (hStable < 255) hStable++; }
      else hStable = 0;
      if (hStable >= RING_STABLE_N) { hRecovered = 0; hStable = 0; hStatus = 1; }
      else hStatus = 0;
    }
  } else { hTripped = 0; hRecovered = 0; hStatus = 0; hStable = 0; }
}

/* Hold one PF for n consecutive cycles; return 1 if the alarm rang at any point. */
static int hystHold(double energy, double limit, double pf, int n) {
  int rang = 0;
  for (int k = 0; k < n; k++) {
    hystStep(energy, limit, pf);
    if (hStatus == 1) rang = 1;
  }
  return rang;
}

int main(void) {
  static float cur[N], volt[N];
  const double dc4A = 4.0 / A_PER_COUNT;      /* the 4.0 A sensor offset */

  printf("1 count = %.4f A at currentCal=100  (float32 accumulators)\n", A_PER_COUNT);

  /* --- 1. a DC offset must not leak into the current reading ------------- */
  printf("\n[1] DC offset trimmed exactly (the user's 4.0 A scenario)\n");
  rngState = 0x12345678u;
  buildCurrent(cur, dc4A, 0.10, 0.8, 0.0);
  near(rmsTwoPass(cur), 0.10, 0.03, "4.0 A offset + 0.10 A load reads 0.10 A");

  rngState = 0x12345678u;
  buildCurrent(cur, 0.0, 0.10, 0.8, 0.0);
  near(rmsTwoPass(cur), 0.10, 0.03, "no offset + 0.10 A load reads the same");

  rngState = 0x12345678u;
  buildCurrent(cur, 2.0 / A_PER_COUNT, 0.10, 0.8, 0.0);
  near(rmsTwoPass(cur), 0.10, 0.03, "a 2.0 A offset changes nothing either");

  /* --- 2. float32 cancellation: one-pass vs two-pass --------------------- */
  printf("\n[2] one-pass cancellation is gone\n");
  double worstOne = 0.0, worstTwo = 0.0;
  const double probes[] = {0.05, 0.10, 0.30, 0.50, 1.00};
  for (unsigned p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
    rngState = 0xABCDEF01u;
    buildCurrent(cur, dc4A, probes[p], 0.0, 0.0);   /* zero ripple: pure signal */
    double one = rmsOnePass(cur), two = rmsTwoPass(cur);
    double e1 = fabs(one - probes[p]), e2 = fabs(two - probes[p]);
    if (e1 > worstOne) worstOne = e1;
    if (e2 > worstTwo) worstTwo = e2;
    printf("     %5.2f A -> one-pass %7.4f A (err %.4f) | two-pass %7.4f A (err %.4f)\n",
           probes[p], one, e1, two, e2);
  }
  near(worstTwo, 0.0, 0.02, "two-pass has no cancellation error left");
  check(worstOne > 0.02,
        "the one-pass form really was broken on these probes (test has teeth)");

  /* --- 3. the dropped noise-power term ----------------------------------- */
  printf("\n[3] a low-PF load keeps its power\n");
  rngState = 0x55AA55AAu;
  buildVoltage(volt, 230.0);
  buildCurrent(cur, dc4A, 0.50, 0.4, 60.0);    /* PF = cos(60 deg) = 0.5 */

  const double floorA = 0.30;
  double curA = trimToNoiseFloor(rmsTwoPass(cur), floorA);
  double wNew = powerNow(volt, cur);
  double wOld = powerOld(volt, cur, floorA);

  check(curA > 0.0, "current survives the floor");
  check(wOld == 0.0, "the OLD term clamped this real power to zero");
  check(wNew > 20.0, "the new maths keeps the power");
  near(wNew, 230.0 * 0.50 * 0.5, 6.0, "power matches V*I*PF");
  printf("     current %.3f A | power new %.1f W | power old %.1f W\n", curA, wNew, wOld);

  /* --- 4. the auto-recover regression ------------------------------------ */
  printf("\n[4] power factor no longer collapses under a load that is ON\n");
  double apparent = 230.0 * curA;
  double pfNew = wNew / apparent;
  double pfOld = wOld / apparent;
  check(pfNew > 0.20, "PF clears AUTO_RECOVER_PF with the load still on");
  check(pfOld < 0.20, "the old maths would have auto-recovered a tripped channel");
  printf("     PF new %.3f | PF old %.3f | threshold 0.20\n", pfNew, pfOld);

  /* --- 5. the floor's own dead zone, pinned as KNOWN behaviour ------------ */
  printf("\n[5] KNOWN: an over-set floor still erases small loads\n");
  for (unsigned f = 0; f < 3; f++) {
    static const double fl[] = {0.10, 0.30, 0.50};
    rngState = 0x77AA77AAu;
    buildCurrent(cur, dc4A, 0.05, 0.4, 0.0);    /* a real 0.05 A load */
    double t = trimToNoiseFloor(rmsTwoPass(cur), fl[f]);
    printf("     floor %.2f A -> a 0.05 A load reads %.3f A%s\n",
           fl[f], t, t == 0.0 ? "  (KILLED)" : "");
  }
  check(trimToNoiseFloor(rmsTwoPass(cur), 0.50) == 0.0,
        "a 0.50 A floor really does erase a 0.05 A load");

  /* --- 6. the no-load deadband: an empty socket reads exactly zero -------- */
  printf("\n[6] KNOWN NOISE: empty socket blanks to P=0/S=0/PF=0, no kWh creep\n");
  rngState = 0x1EA1BE1u;
  buildVoltage(volt, 230.0);
  for (int i = 0; i < N; i++)              /* current sensor, nothing plugged */
    cur[i] = clampCounts(BIAS + noise(0.8));
  {
    double rawI = trimToNoiseFloor(rmsTwoPass(cur), 0.0);
    double rawP = powerNow(volt, cur);
    double s, pf;
    double gatedP = gateLoad(rawI, rawP, 230.0, &s, &pf);
    printf("     raw I %.3f A | raw |P| %.2f W -> gated P %.1f W S %.1f VA PF %.3f\n",
           rawI, rawP, gatedP, s, pf);
    check(rawP < NO_LOAD_P,
          "the power gate is what blanks idle noise (test has teeth)");
    check(gatedP == 0.0 && s == 0.0 && pf == 0.0,
          "idle noise blanks to P=0/S=0/PF=0");
    double kwh = 0.0;                       /* 1000 sensor cycles add nothing */
    for (int k = 0; k < 1000; k++) kwh += gatedP * (0.08 / 3600.0) / 1000.0;
    check(kwh == 0.0, "no kWh creep from an empty socket");
  }

  /* --- 7. the deadband boundary: small real loads ------------------------- */
  printf("\n[7] a 0.02 A load is blanked, a 0.10 A load passes\n");
  rngState = 0xB0A7DA1u;
  buildCurrent(cur, dc4A, 0.02, 0.4, 0.0);
  {
    double rawI = trimToNoiseFloor(rmsTwoPass(cur), 0.0);
    double rawP = powerNow(volt, cur);
    double s, pf;
    double gatedP = gateLoad(rawI, rawP, 230.0, &s, &pf);
    printf("     0.02 A -> raw I %.3f A P %.2f W -> gated %.1f W\n", rawI, rawP, gatedP);
    check(gatedP == 0.0, "a 0.02 A trickle does not register (documented cost)");
  }
  rngState = 0xB0A7DA2u;
  buildCurrent(cur, dc4A, 0.10, 0.4, 0.0);
  {
    double rawI = trimToNoiseFloor(rmsTwoPass(cur), 0.0);
    double rawP = powerNow(volt, cur);
    double s, pf;
    double gatedP = gateLoad(rawI, rawP, 230.0, &s, &pf);
    printf("     0.10 A -> raw I %.3f A P %.2f W -> gated %.1f W PF %.3f\n",
           rawI, rawP, gatedP, pf);
    check(gatedP > 0.0, "a 0.10 A load survives the deadband");
    near(gatedP, 230.0 * 0.10, 4.0, "0.10 A resistive reads ~23 W");
    check(pf > 0.5, "its PF is nowhere near the recover band");
  }

  /* --- 8. ring persistence: a recovered trip needs PF > 0.2 for ~1.5 s -- */
  printf("\n[8] spikes never re-ring; 19 steady cycles above 0.2 do\n");
  hystReset();
  hystStep(50.0, 48.0, 0.80);               /* over budget, load on: TRIPPED */
  check(hStatus == 1, "over budget with load on rings");
  hystStep(50.0, 48.0, 0.10);               /* load removed: recovers, silent */
  check(hStatus == 0, "load removed silences the alarm");
  {
    /* The user's phantom: empty socket, PF flickering, brief stabs over 0.2. */
    static const double spikes[] = {0.0, 0.21, 0.0, 0.5, 0.5, 0.0, 0.35, 0.0, 0.28};
    int rang = 0;
    for (unsigned k = 0; k < sizeof(spikes) / sizeof(spikes[0]); k++) {
      hystStep(50.0, 48.0, spikes[k]);
      if (hStatus == 1) rang = 1;
    }
    check(!rang, "brief stabs over 0.2 never re-ring the recovered trip");
  }
  check(!hystHold(50.0, 48.0, 0.60, RING_STABLE_N - 1),
        "18 steady cycles at 0.6 PF: still silent (one short of proof)");
  check(hystHold(50.0, 48.0, 0.60, 1),
        "the 19th steady cycle re-arms the alarm");
  check(hStatus == 1, "alarm is TRIPPED after ~1.5 s of steady load");
  hystStep(50.0, 48.0, 0.10);
  check(hStatus == 0, "falling under 0.2 silences instantly");
  check(hystHold(50.0, 48.0, 0.25, RING_STABLE_N),
        "a steady 0.25 PF (just over the line) still re-rings after ~1.5 s");
  hystStep(50.0, 48.0, 0.19);
  check(hStatus == 0, "0.19 is below the line: silent");
  hystStep(10.0, 48.0, 0.90);
  check(hStatus == 0, "back under budget clears everything");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

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
  check(pfNew > 0.30, "PF clears AUTO_RECOVER_PF with the load still on");
  check(pfOld < 0.30, "the old maths would have auto-recovered a tripped channel");
  printf("     PF new %.3f | PF old %.3f | threshold 0.30\n", pfNew, pfOld);

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

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

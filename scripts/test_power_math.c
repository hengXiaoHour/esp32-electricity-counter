/*
 * Host regression test for the current/power trim maths in
 * src/core/power_calculator.cpp::computeAll().
 *
 * Why this exists (2026-10-09): the board trimmed the sensor's DC offset by
 * taking a variance, which is correct, but two real defects sat on top of it:
 *
 *   1. The variance was one-pass: E[X^2] - E[X]^2. At the ~2048-count
 *      mid-supply bias, E[X^2] ~ 4.19e6 where one float ULP is 0.5 counts^2,
 *      while a 0.01 A load has a variance of ~0.01 counts^2. Small signals came
 *      out as rounding noise.
 *   2. Active power subtracted `voltageRMS * noiseFloor` even though pWatts is
 *      a covariance of two mean-removed signals and already excludes the DC
 *      offset. The two trims disagreed, so a channel could show 0.62 A of
 *      current next to 0 W of power.
 *
 * These are the exact expressions from computeAll(), transcribed so the maths
 * can be exercised on a host without hardware. Negative controls run last: if
 * the one-pass or the noise-power term is ever put back, they must FAIL.
 *
 * Build:  cc -O2 -o /tmp/pctest test_power_math.c -lm && /tmp/pctest
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define N            1000
#define ADC_MAX      4095.0
#define VREF         3.3
#define CURRENT_CAL  100.0
#define BIAS         2048.0

/* 1 count at currentCal=100 is this many amps. */
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

/* xorshift so the fixtures are byte-identical on every host. */
static uint32_t rngState = 0x12345678u;
static double nextGaussish(double sigma) {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  double u = ((double)(rngState & 0xFFFFFF) / (double)0xFFFFFF) - 0.5;
  return u * 2.0 * sigma * 1.732;   /* uniform -> roughly gaussian */
}

/* voltageCal as configured in src/config.h */
static const double VOLTAGE_CAL = 260.0;

/* signalA is an RMS value, so the sine peak is RMS * sqrt(2). Getting this
 * wrong silently divides every reading by sqrt(2). */
static void buildSamples(double *buf, double dcCounts, double signalA,
                         double rippleCounts) {
  for (int i = 0; i < N; i++) {
    double t = 2.0 * M_PI * i / N;
    double peakCounts = (signalA / A_PER_COUNT) * M_SQRT2;
    double v = BIAS + dcCounts + peakCounts * sin(t) + nextGaussish(rippleCounts);
    if (v < 0.0) v = 0.0;
    if (v > ADC_MAX) v = ADC_MAX;
    buf[i] = v;
  }
}

/* Mains voltage channel: RMS volts -> ADC counts, biased to mid-supply. */
static void buildVoltage(double *buf, double rmsVolts) {
  double adcRms = (rmsVolts / VOLTAGE_CAL) / VREF * ADC_MAX;
  double peak = adcRms * M_SQRT2;
  for (int i = 0; i < N; i++) {
    double t = 2.0 * M_PI * i / N;
    double v = BIAS + peak * sin(t) + nextGaussish(1.0);
    if (v < 0.0) v = 0.0;
    if (v > ADC_MAX) v = ADC_MAX;
    buf[i] = v;
  }
}

/* ---- the two variance forms, as they appear in the firmware ------------- */

static double rmsTwoPass(const double *x) {          /* what the board does NOW */
  double mean = 0.0;
  for (int i = 0; i < N; i++) mean += x[i];
  mean /= N;
  double acc = 0.0;
  for (int i = 0; i < N; i++) { double d = x[i] - mean; acc += d * d; }
  return sqrt(acc / N) / ADC_MAX * VREF * CURRENT_CAL;
}

static double rmsOnePass(const double *x) {          /* the OLD form */
  double isum = 0.0, isq = 0.0;
  for (int i = 0; i < N; i++) { isum += x[i]; isq += x[i] * x[i]; }
  double mean = isum / N, meansq = isq / N;
  return sqrt(meansq - mean * mean) / ADC_MAX * VREF * CURRENT_CAL;
}

static double trimToNoiseFloor(double raw, double floorA) {
  return (raw * raw > floorA * floorA) ? sqrt(raw * raw - floorA * floorA) : 0.0;
}

/* active power, current code: covariance only, no noise-power subtraction. */
static double powerNow(const double *v, const double *i) {
  double vMean = 0.0, iMean = 0.0;
  for (int k = 0; k < N; k++) { vMean += v[k]; iMean += i[k]; }
  vMean /= N; iMean /= N;
  double pSum = 0.0;
  for (int k = 0; k < N; k++)
    pSum += (v[k] - vMean) * (i[k] - iMean);
  double pMean = pSum / N;
  double adcToVolt = VREF / ADC_MAX;
  return fabs(pMean * adcToVolt * adcToVolt * 260.0 * CURRENT_CAL);
}

/* active power, OLD code: the bogus voltageRMS * floor term. */
static double powerOld(const double *v, const double *i, double floorA) {
  double p = powerNow(v, i);
  double vMean = 0.0;
  for (int k = 0; k < N; k++) vMean += v[k];
  vMean /= N;
  double acc = 0.0;
  for (int k = 0; k < N; k++) { double d = v[k] - vMean; acc += d * d; }
  double vRms = sqrt(acc / N) / ADC_MAX * VREF * 260.0;
  double noisePower = vRms * floorA;
  double out = p - noisePower;
  return out < 0.0 ? 0.0 : out;
}

int main(void) {
  static double cur[N], volt[N];
  const double dc4A = 4.0 / A_PER_COUNT;   /* the user's 4.0 A offset */

  printf("1 count = %.4f A at currentCal=100\n", A_PER_COUNT);

  /* --- 1. a DC offset must not leak into the current reading ------------- */
  printf("\n[1] DC offset of 4.0 A with a 0.10 A real load\n");
  rngState = 0x12345678u;
  buildSamples(cur, dc4A, 0.10, 0.8);
  near(rmsTwoPass(cur), 0.10, 0.05, "two-pass recovers the load, offset trimmed");

  /* same signal, 10x the offset: the reading must not move */
  rngState = 0x12345678u;
  static double bigDc[N];
  buildSamples(bigDc, 50.0, 0.10, 0.8);   /* ~4 A of offset, same place */
  near(rmsTwoPass(bigDc), 0.10, 0.05, "reading independent of the DC offset");

  /* --- 2. two-pass vs one-pass on small signals --------------------------- */
  printf("\n[2] one-pass cancellation is fixed\n");
  double worstOld = 0.0;
  const double probes[] = {0.05, 0.10, 0.30, 0.50};
  for (unsigned p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
    rngState = 0xABCDEF01u;
    buildSamples(cur, dc4A, probes[p], 0.0);   /* zero ripple: pure signal */
    double two = rmsTwoPass(cur), one = rmsOnePass(cur);
    near(two, probes[p], probes[p] * 0.25, "two-pass tracks the probe");
    double errOne = fabs(one - probes[p]);
    if (errOne > worstOld) worstOld = errOne;
    printf("     probe %.2f A -> one-pass %.4f A, two-pass %.4f A\n",
           probes[p], one, two);
  }
  check(worstOld > 0.10,
        "the one-pass form was genuinely broken on these probes");

  /* --- 3. current and power must agree ------------------------------------ */
  printf("\n[3] a floored channel shows current AND power together\n");
  rngState = 0x55AA55AAu;
  buildVoltage(volt, 230.0);
  buildSamples(cur, dc4A, 0.50, 0.8);

  const double floorA = 0.30;
  double curA = trimToNoiseFloor(rmsTwoPass(cur), floorA);
  double wNew = powerNow(volt, cur);
  double wOld = powerOld(volt, cur, floorA);

  check(curA > 0.0, "the 0.50 A load survives a 0.30 A floor");
  check(wNew > 0.0, "the same load registers power (was clamped to 0)");
  check(wOld == 0.0,
        "the OLD noise-power term really did clamp this to zero");
  printf("     current %.3f A | power new %.1f W | power old %.1f W\n",
         curA, wNew, wOld);
  check(wNew > 100.0 && wNew < 150.0,
        "power is now in the physically sensible range for 230V x 0.5A");

  /* --- 4. power factor stays meaningful (the auto-recover regression) ----- */
  printf("\n[4] power factor no longer collapses to 0 while current flows\n");
  double apparent = 230.0 * curA;
  double pf = wNew / apparent;
  check(pf > 0.3, "PF clears AUTO_RECOVER_PF with the load still on");
  check(powerOld(volt, cur, floorA) / apparent < 0.3,
        "the old maths would have auto-recovered a tripped channel");
  printf("     PF new %.3f (threshold 0.3), PF old %.3f\n",
         pf, wOld / apparent);

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

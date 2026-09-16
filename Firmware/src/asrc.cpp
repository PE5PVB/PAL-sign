#include "asrc.h"

#include "asrc_taps.h"
#include "asrc_taps_32.h"
#include "asrc_taps_16.h"
#include "pcm1808.h"

// Input history, one ring per channel, ASRC_TAPS deep. Power of two so
// the index wraps with a mask.
static_assert((ASRC_TAPS & (ASRC_TAPS - 1)) == 0, "ASRC_TAPS must be a power of two");
static int16_t histL[ASRC_TAPS];
static int16_t histR[ASRC_TAPS];
static uint32_t histPos = 0;  // where the newest input pair sits
static int16_t lastL = 0, lastR = 0;

// The conversion ratio, input pairs per output pair, in Q24: 48000 /
// 44100 = 1.0884 nominal. `phase` is the position of the next output
// between input pairs, also Q24; its top bits pick the filter phase.
static const uint32_t ONE_Q24 = 1u << 24;
static uint32_t STEP_NOMINAL = (uint32_t)(48000.0 / 44100.0 * (double)ONE_Q24 + 0.5);
static uint32_t stepBase = STEP_NOMINAL;  // the I term: the learned rate difference
static uint32_t step = STEP_NOMINAL;
static uint32_t phase = 0;
// The tap set for the output rate: 32 taps for 44.1 kHz, 16 for 32 kHz
// (make_asrc.py on why), addressed as phase * stride.
static const int16_t *coef = &ASRC_COEF[0][0];
static int coefTaps = ASRC_TAPS;

// Input queue kept in pcm1808's pair ring (192 pairs): this only takes
// what it needs, so the fill level there is the steering signal. It
// is read once a field, on the control line, which is right after the
// ~18 lines a field that carry no data: the PEAK of the level, ~55
// pairs above the trough at the end of the data lines. 96 keeps the
// trough near 40 and the peak well under the ring. Holding the peak
// at 64 in a 128 ring left a trough of ~9 pairs, and every bit of
// jitter ran the input dry: a held sample 50 times a second, heard as
// stuttering (measured with a counter on it, since removed).
static const int LEVEL_TARGET = 96;

void asrcReset(int outHz) {
  STEP_NOMINAL = (uint32_t)(48000.0 / (double)outHz * (double)ONE_Q24 + 0.5);
  coef = outHz == 32000 ? &ASRC_COEF_32[0][0] : outHz == 16000 ? &ASRC_COEF_16[0][0] : &ASRC_COEF[0][0];
  coefTaps = outHz == 32000 ? ASRC_TAPS_32 : outHz == 16000 ? ASRC_TAPS_16 : ASRC_TAPS;
  for (int i = 0; i < ASRC_TAPS; i++) histL[i] = histR[i] = 0;
  histPos = 0;
  lastL = lastR = 0;
  stepBase = step = STEP_NOMINAL;
  phase = 0;
}

static inline void pushInput() {
  int16_t l, r;
  if (pcm1808TakePairs(&l, &r, 1) == 1) {
    lastL = l;
    lastR = r;
  }
  // Dry: hold the last pair rather than stall the line.
  histPos = (histPos + 1) & (ASRC_TAPS - 1);
  histL[histPos] = lastL;
  histR[histPos] = lastR;
}

void asrcPull(int16_t *l, int16_t *r, int n) {
  for (int i = 0; i < n; i++) {
    while (phase >= ONE_Q24) {
      pushInput();
      phase -= ONE_Q24;
    }
    // Filter phase from the fractional position; tap k reaches k pairs
    // back from the newest.
    const int16_t *h = coef + (phase >> (24 - 5)) * coefTaps;  // 32 phases
    // 32 bit accumulators, deliberately: a 64 bit multiply-accumulate
    // goes through a library routine on this core and cost ~40 us for
    // the 192 taps of one line's three samples, over the row budget
    // (the picture starved on the first try). 32 bit is one
    // instruction a tap and cannot overflow: the largest sum of |h| in
    // any phase of asrc_taps.h is 60439 (make_asrc.py prints it), times
    // a full-scale 32767 is 1.98e9, under 2^31.
    int32_t accL = 0, accR = 0;
    uint32_t p = histPos;
#pragma GCC unroll 8
    for (int k = 0; k < coefTaps; k++) {
      accL += (int32_t)h[k] * histL[p];
      accR += (int32_t)h[k] * histR[p];
      p = (p - 1) & (ASRC_TAPS - 1);
    }
    int32_t oL = accL >> 15;
    int32_t oR = accR >> 15;
    if (oL > 32767) oL = 32767; else if (oL < -32768) oL = -32768;
    if (oR > 32767) oR = 32767; else if (oR < -32768) oR = -32768;
    l[i] = (int16_t)oL;
    r[i] = (int16_t)oR;
    phase += step;
  }
}

int asrcAvailable() {
  // Input pairs still queued over the step, in 32 bit (a 64 bit divide
  // is a library call on this core): the queue is at most a few hundred
  // pairs and the step at most 2 in Q24, so both fit after the shifts.
  const uint32_t q = ((uint32_t)pcm1808Available() << 12) - (phase >> 12);
  return (int32_t)q <= 0 ? 0 : (int)(q / (step >> 12));
}

void asrcSteer() {
  // P holds the level, I learns the real rate difference between the
  // ADC's clock and the video's. Gains per pair of error, per field.
  // Both small: a ratio change is a pitch change.
  int err = pcm1808Available() - LEVEL_TARGET;
  // I: 2e-7 of the nominal step per pair of error, per field.
  int64_t di = (int64_t)err * (int64_t)STEP_NOMINAL / 5000000;
  stepBase = (uint32_t)((int64_t)stepBase + di);
  // P: 1e-5 of the nominal step per pair of error.
  int64_t dp = (int64_t)err * (int64_t)STEP_NOMINAL / 100000;
  int64_t s = (int64_t)stepBase + dp;
  const int64_t lo = (int64_t)STEP_NOMINAL * 98 / 100;
  const int64_t hi = (int64_t)STEP_NOMINAL * 102 / 100;
  if (s < lo) s = lo;
  if (s > hi) s = hi;
  if ((int64_t)stepBase < lo) stepBase = (uint32_t)lo;
  if ((int64_t)stepBase > hi) stepBase = (uint32_t)hi;
  step = (uint32_t)s;
}

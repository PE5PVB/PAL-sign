#include "movingline.h"

#include "gfx.h"
#include "settings.h"

// Full white plus a blended sample at each edge; 4 at 13.5 MHz is 296 ns.
static const int LINE_W = 4;

// A sweep is 50 fields, not 25 frames: the two fields of a frame go out
// 20 ms apart, so the stripe steps between them. videoBuildLine() maps
// y = activeY * 2 + (field2 ? 1:0), so even rows are field 1, odd rows
// 20 ms later; drawFp[] below is indexed the same way. A field is
// 312.5 lines of 64 us = 20.000 ms, so 50 fields is one second.
static const int SWEEP_FIELDS = 50;
static const int PERIOD_FIELDS = 2 * SWEEP_FIELDS;

static bool inUse = false;
static bool paused = false;
static int bx0, bx1, by0, by1;  // the declared box, [x0,x1) x [y0,y1)
static int phase = 0;           // counts fields, so +2 per picture

// What movingLineDrawRow() reads, refreshed once per picture in
// movingLineFrame(): stripe position per field, in 1/16 sample.
static bool drawOn = false;
static int drawFp[2];

void movingLineBox(int x0, int x1, int y0, int y1) {
  inUse = true;
  bx0 = x0;
  bx1 = x1;
  by0 = y0;
  by1 = y1;
}

void movingLineClearInUse() {
  inUse = false;
  drawOn = false;
}

bool movingLineInUse() { return inUse; }

void movingLineGetBox(int *x0, int *x1, int *y0, int *y1) {
  *x0 = bx0;
  *x1 = bx1;
  *y0 = by0;
  *y1 = by1;
}

void movingLinePause(bool v) { paused = v; }

bool movingLineActive() { return drawOn && !paused; }

void movingLineFrame() {
  if (!inUse || paused || !cfgCardMovingLine(-1)) {
    drawOn = false;
    phase = 0;
    return;
  }
  int travelFp = ((bx1 - LINE_W) - bx0) * 16;
  if (travelFp < 0) travelFp = 0;
  const int steps = SWEEP_FIELDS - 1;
  for (int fld = 0; fld < 2; fld++) {
    // Fold the field count into a triangle: up one leg, mirrored down
    // the other.
    const int ph = (phase + fld) % PERIOD_FIELDS;
    const int f = (ph < SWEEP_FIELDS) ? ph : (PERIOD_FIELDS - 1) - ph;
    drawFp[fld] = bx0 * 16 + (travelFp * f + steps / 2) / steps;
  }
  drawOn = true;
  phase = (phase + 2) % PERIOD_FIELDS;
}

// Pulls luma towards peak white by cov16/16, never down; luma only,
// which is why the box needs neutral chroma.
static inline void lumaTowardsWhite(uint8_t *base, int x, int cov16) {
  if (x < 0 || x >= SCREEN_W || cov16 <= 0) return;
  const int idx = (x >> 1) * 4 + ((x & 1) ? 3 : 1);
  const int d = 235 - base[idx];
  if (d <= 0) return;
  base[idx] = (uint8_t)(base[idx] + (d * cov16) / 16);
}

void movingLineDrawRow(uint8_t *base, int y) {
  if (!drawOn || paused || y < by0 || y >= by1) return;
  // The two edge samples take luma in proportion to how much of them
  // the pulse covers, so a 1/16 sample step reads as smooth motion.
  const int fp = drawFp[y & 1];
  const int ix = fp >> 4;
  const int frac = fp & 15;
  lumaTowardsWhite(base, ix, 16 - frac);
  for (int i = 1; i < LINE_W; i++) lumaTowardsWhite(base, ix + i, 16);
  lumaTowardsWhite(base, ix + LINE_W, frac);
}

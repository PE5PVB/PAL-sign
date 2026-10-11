// BATC card: a frequency staircase on top, the station's call (the
// first text line) large in the middle beside colour bars, and reversed
// bars with a grey staircase below. Laid out after a repeater card
// (GB3VL), measured from a photograph of it.
//
// The staircase is a binary counter across the line: seven rows, each one
// bit of a count that steps every 1.6875 samples, the finest row a 4 MHz
// square wave (13.5 MHz / 2 / 1.6875), each row above half the frequency
// down to 62.5 kHz; 416 steps, exactly the 702 samples of the active
// line (52 us). Edges are anti-aliased by area, so no row aliases.
//
// Full picture: the bands share all 576 rows and every band runs from one
// edge of the active line (samples 9..711) to the other.
//
// Upper bars: white 100 %, then yellow, cyan, green, magenta, red, blue at
// 75 %. Lower: the six colours reversed, then grey 100 % (half width) and
// 87.5 % down to 12.5 % in steps of 12.5 %.
//
// The call is lettered in Mixed bars' face, Inter, whatever the card's
// text font, as large as fits its box: laid out from a 110 row face
// (BATC_FONT, runs like the contest page's) and scaled down by area
// once at layout, so the letters keep their smooth, anti-aliased round
// edges at any length of call.
//
// Every row of the card is one of nine, worked out once into the ROM cards'
// run table (romBorrowScratch(), idle while this card is on); a row is a
// copy plus the text, whose layout lives in that table too.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "../gfx.h"
#include "../language.h"
#include "../pattern.h"
#include "../rompattern.h"
#include "../settings.h"
#include "../textoverlay.h"
#include "interline.h"

static const int ROW_BYTES = SCREEN_W * 2;

// Rows of the staircase, top to bottom (picture rows 0..575).
static const int STAIR_Y[8] = {0, 42, 84, 126, 168, 210, 252, 294};
static const int STAIR_ROWS = 7;
static const float STEP = 1.6875f;   // samples a count step: 4 MHz at the bottom
static const int STEPS = 416;   // 416 x 1.6875 = 702
static const int ACTIVE_X0 = 9, ACTIVE_X1 = 711;   // BT.601's 702 active samples

// The middle band: bars right, the call left.
static const int MID_Y0 = 296, MID_Y1 = 436;
static const int MID_BAR_W = 34;                      // seven bars, ending at the right edge
static const int MID_BAR_X0 = ACTIVE_X1 - 7 * MID_BAR_W;
static const int TEXT_X0 = 60, TEXT_X1 = 458;   // the call's box, clear of overscan and the bars
static const int TEXT_Y0 = 300, TEXT_Y1 = 432;

// The lower band: six bars, then the grey steps.
static const int LOW_Y0 = 440, LOW_Y1 = SCREEN_H;
static const int LOW_BAR_X0 = ACTIVE_X0, LOW_BAR_W = 32;
static const int GREY_W = 68;   // 6 x 32 + 34 + 7 x 68 = 702

// The call as coverage, scaled to fit, in the borrowed table after the
// rows; w x h bytes from (x, y).
struct CallMap {
  int x, y, w, h;
  InterInk ink;
};

static uint8_t *rows;       // nine full rows in the borrowed table
static CallMap *call;       // also there
static uint8_t *callCov;    // and the coverage after it
static uint8_t *scratchEnd;

// sRGB at 75 % (191) for the colours, as EBU 100/0/75/0 bars.
static const uint32_t BAR75[] = {0xBFBF00u, 0x00BFBFu, 0x00BF00u, 0xBF00BFu, 0xBF0000u, 0x0000BFu};

static void fillRow(uint8_t *r, int x0, int x1, YCbCr c) {
  if (x0 < 0) x0 = 0;
  if (x1 > SCREEN_W) x1 = SCREEN_W;
  for (int x = x0; x < x1; x++) {
    r[2 * x + 1] = c.Y;
    if ((x & 1) == 0) {
      r[2 * x] = c.Cb;
      r[2 * x + 2] = c.Cr;
    }
  }
}

static void blackRow(uint8_t *r) {
  uint32_t *w = (uint32_t *)r;
  for (int i = 0; i < SCREEN_W / 2; i++) w[i] = 0x10801080u;
}

// One staircase row: the count's bit of weight `half` (in steps), white
// where it is set, each pixel the share of its width that is white.
static void stairRow(uint8_t *r, int half) {
  blackRow(r);
  const float x0 = (float)ACTIVE_X0;
  for (int x = 0; x < SCREEN_W; x++) {
    float white = 0.0f;
    // A pixel spans [x, x+1): walk the count steps it overlaps.
    const float u0 = ((float)x - x0) / STEP, u1 = ((float)x + 1.0f - x0) / STEP;
    for (int u = (int)floorf(u0); u <= (int)floorf(u1); u++) {
      if (u < 0 || u >= STEPS || ((u / half) & 1) == 0) continue;
      const float a = fmaxf(u0, (float)u), b = fminf(u1, (float)(u + 1));
      if (b > a) white += (b - a) * STEP;
    }
    r[2 * x + 1] = (uint8_t)(16.0f + 219.0f * fminf(white, 1.0f) + 0.5f);
  }
}

static void middleRow(uint8_t *r) {
  blackRow(r);
  fillRow(r, MID_BAR_X0, MID_BAR_X0 + MID_BAR_W, yccFromRgb(0xFFFFFFu));
  for (int i = 0; i < 6; i++) {
    const int x = MID_BAR_X0 + (i + 1) * MID_BAR_W;
    fillRow(r, x, x + MID_BAR_W, yccFromRgb(BAR75[i]));
  }
}

static void lowerRow(uint8_t *r) {
  blackRow(r);
  for (int i = 0; i < 6; i++) {
    const int x = LOW_BAR_X0 + i * LOW_BAR_W;
    fillRow(r, x, x + LOW_BAR_W, yccFromRgb(BAR75[5 - i]));
  }
  int x = LOW_BAR_X0 + 6 * LOW_BAR_W;
  fillRow(r, x, x + GREY_W / 2, yccFromRgb(0xFFFFFFu));
  x += GREY_W / 2;   // 34
  for (int step = 7; step >= 1; step--) {
    const uint8_t v = (uint8_t)(255 * step / 8);
    fillRow(r, x, x + GREY_W, yccFromRgb(((uint32_t)v << 16) | ((uint32_t)v << 8) | v));
    x += GREY_W;
  }
}

// The light half of the init: the call laid out and scaled down by area
// into the coverage map, as large as the box allows (see interline.h). A
// once-only cost of a few tens of ms, at a card change or a text edit.
static void batcReflow() {
  if (!call) return;
  interInkBuild(&call->ink, textInkId());
  const int boxW = TEXT_X1 - TEXT_X0, boxH = TEXT_Y1 - TEXT_Y0;
  interLineLayout(cfgTextIdShown(), boxW, boxH, callCov, scratchEnd, &call->w, &call->h);
  call->x = TEXT_X0 + (boxW - call->w) / 2;
  call->y = TEXT_Y0 + (boxH - call->h) / 2;
}

// A colour change on the card in view refreshes the ink, once a picture.
static void batcTick() {
  if (!call) return;
  const YCbCr now = textInkId();
  if (now.Y != call->ink.ink.Y || now.Cb != call->ink.ink.Cb || now.Cr != call->ink.ink.Cr)
    interInkBuild(&call->ink, now);
}

static void batcInit() {
  uint32_t bytes = 0;
  uint8_t *scratch = romBorrowScratch(&bytes);
  if (bytes < 9 * ROW_BYTES + sizeof(CallMap) + 64 * 1024) {
    rows = nullptr;
    call = nullptr;
    return;
  }
  rows = scratch;
  for (int i = 0; i < STAIR_ROWS; i++) stairRow(rows + i * ROW_BYTES, 64 >> i);
  middleRow(rows + 7 * ROW_BYTES);
  lowerRow(rows + 8 * ROW_BYTES);
  call = (CallMap *)(scratch + 9 * ROW_BYTES);
  callCov = (uint8_t *)(call + 1);
  scratchEnd = scratch + bytes;
  batcReflow();
}

static void batcRenderRow(uint8_t *base, int y) {
  if (!rows) {
    blackRow(base);
    return;
  }
  const uint8_t *src = nullptr;
  for (int i = 0; i < STAIR_ROWS; i++) {
    if (y >= STAIR_Y[i] && y < STAIR_Y[i + 1]) src = rows + i * ROW_BYTES;
  }
  if (y >= MID_Y0 && y < MID_Y1) src = rows + 7 * ROW_BYTES;
  if (y >= LOW_Y0 && y < LOW_Y1) src = rows + 8 * ROW_BYTES;
  if (src) memcpy(base, src, ROW_BYTES);
  else blackRow(base);
  const int r = y - call->y;
  if (r < 0 || r >= call->h) return;
  interDrawRow(base, call->x, callCov + r * call->w, call->w, &call->ink);
}

const Pattern PATTERN_BATC = {LANG_CARD_BATC, batcInit, batcRenderRow, batcTick,
                                  false, false, batcReflow};

// BATC Legacy: an amateur TV test card from a BBC Micro program (the
// G7GTN version of the BATC book's "Test Card with Call sign"), laid out
// by eye from a photograph of a screen, since the program itself is not
// to hand. The call is the first text line, large; the line under the
// colour bars is the second text line, with a third line of its own
// below it (cfgLegacyText(), "TV TEST TRANSMISSION." from the factory). The call is lettered in Mixed bars' Inter, scaled to fill its
// box (interline.h); the two lines under the bars follow the card's text
// font, left aligned.
//
// All positions are in pixels of that photograph (317 x 241), which fills
// the active picture (702 samples from 9, 576 rows), shifted right so the
// layout is centred (it spans x 5..279 in the photograph). The grid is
// Crosshatch's (card 1's own grid), white lines two samples and two rows
// wide; of the coloured blocks along the top only the three on the white
// panels stay (the photograph's blue reads as purple there).
//
// Everything but the lettering is the same for every row of a band, so the
// rows are composed once into the ROM cards' run table (romBorrowScratch(),
// idle while this card is on), one copy per distinct row; a row is then a
// copy plus the letters crossing it.
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
static const float PHOTO_W = 317.0f, PHOTO_H = 241.0f;
static const int ACTIVE_X0 = 9, ACTIVE_W = 702;

// Photograph x to picture sample (centred), photograph y to picture row
// (raised, to leave room under the two lines at the bottom).
static const float CENTRE_SHIFT = PHOTO_W / 2 - (5.0f + 279.0f) / 2;
static const float RAISE = 10.0f;
static int vx(float x) { return ACTIVE_X0 + (int)((x + CENTRE_SHIFT) * ACTIVE_W / PHOTO_W + 0.5f); }
static int vy(float y) { return (int)((y - RAISE) * SCREEN_H / PHOTO_H + 0.5f); }

enum { K, W, C, B, R, Y, G, M, P };   // black, white, cyan, blue, red, yellow, green, magenta, purple
static const uint32_t RGB[] = {0x000000u, 0xFFFFFFu, 0x00FFFFu, 0x0000FFu, 0xFF0000u,
                               0xFFFF00u, 0x00FF00u, 0xFF00FFu, 0x8000FFu};

// A block, in photograph pixels, x0..x1 by y0..y1, drawn in this order.
struct Block {
  float x0, x1, y0, y1;
  uint8_t c;
};
static const Block BLOCKS[] = {
    // The call's box, from the grid line under the panels' teeth.
    {55, 235, 103.3f, 147, K},
    // The gratings' band (the bars themselves follow below).
    {8, 272, 147, 177, K},
    // Colour bars.
    {5, 50, 179, 203, C}, {50, 97, 179, 203, G}, {97, 142, 179, 203, M}, {142, 186, 179, 203, R},
    {186, 230, 179, 203, B}, {230, 272, 179, 203, K},
    // The text box.
    {55, 235, 203, 238, K},
};

// The top, in grid cells (VLINES and HLINES indices), so its edges are
// the grid's lines: a block runs from line x0 to line x1, y0 to y1,
// covering both.
struct Cells {
  uint8_t x0, x1, y0, y1, c;
};
static const Cells TOP[] = {
    // Purple, red, yellow on top of the white panels.
    {4, 7, 1, 2, P}, {7, 10, 1, 2, R}, {10, 13, 1, 2, Y},
    // The white panels, with their teeth hanging down.
    {1, 6, 2, 4, W}, {2, 3, 4, 5, W}, {5, 6, 4, 5, W},
    {11, 16, 2, 4, W}, {13, 14, 4, 5, W}, {15, 16, 4, 5, W},
    // The black centre and its two white squares.
    {6, 11, 2, 5, K}, {7, 8, 4, 5, W}, {10, 11, 4, 5, W},
};
// The white needle in the black centre, in picture samples and rows.
static const int NEEDLE_X0 = 289, NEEDLE_X1 = 296;

// Grating sections: white bars of half the period, from x0 to x1.
struct Grating {
  float x0, x1, period;
};
static const Grating GRATINGS[] = {{8, 54, 16}, {54, 140, 8}, {140, 188, 4}, {188, 272, 2}};

// The grid, in picture samples and rows: Crosshatch's lines.
static const int GRID_T = 2;
static const int HLINES[] = {13, 55, 97, 139, 181, 223, 265, 307, 349, 391, 433, 475, 517, 559};
static const int VLINES[] = {32,  70,  109, 147, 186, 224, 263, 301, 340,
                             378, 416, 455, 493, 532, 570, 609, 647, 686};

// Lettering boxes, in photograph pixels: the call (centred) and the two
// lines under the bars (left aligned).
struct TextBox {
  float x0, x1, y0, y1;
};
static const TextBox CALL_BOX = {55, 235, 105, 145};
static const TextBox LINE_BOX[2] = {{62, 235, 204, 217}, {62, 235, 223, 236}};
static const float CALL_SQUEEZE = 0.75f;   // the call's letters, narrower than Inter's own

// The call, as Inter coverage w x h from (x, y), and the two lines.
struct Lettering {
  int x, y, w, h;
  uint8_t *cov;
  InterInk ink;
  TextOverlay line[2];
};

static uint8_t *rows;       // distinct composed rows
static uint8_t *rowOf;      // picture row -> composed row
static Lettering *letters;
static uint8_t *freeMem, *scratchEnd;   // where the call's coverage goes

static void fill(uint8_t *r, int x0, int x1, uint8_t c) {
  if (x0 < 0) x0 = 0;
  if (x1 > SCREEN_W) x1 = SCREEN_W;
  if (x0 >= x1) return;
  fillSpanSolid(r, x0, x1, yccFromRgb(RGB[c]));
}

static void composeRow(int y, uint8_t *r) {
  uint32_t *w = (uint32_t *)r;
  for (int i = 0; i < SCREEN_W / 2; i++) w[i] = 0x10801080u;
  // The grid.
  bool hline = false;
  for (int h : HLINES) hline |= y >= h && y < h + GRID_T;
  if (hline) fill(r, 0, SCREEN_W, W);
  for (int v : VLINES) fill(r, v, v + GRID_T, W);
  // The top.
  for (const Cells &b : TOP) {
    if (y >= HLINES[b.y0] && y < HLINES[b.y1] + GRID_T)
      fill(r, VLINES[b.x0], VLINES[b.x1] + GRID_T, b.c);
  }
  if (y >= HLINES[2] + 2 * GRID_T && y < HLINES[4]) fill(r, NEEDLE_X0, NEEDLE_X1, W);
  // The blocks.
  for (const Block &b : BLOCKS) {
    if (y >= vy(b.y0) && y < vy(b.y1)) fill(r, vx(b.x0), vx(b.x1), b.c);
  }
  // The gratings.
  if (y >= vy(147) && y < vy(177)) {
    for (const Grating &g : GRATINGS) {
      for (float x = g.x0; x < g.x1; x += g.period) {
        const float e = x + g.period / 2 < g.x1 ? x + g.period / 2 : g.x1;
        fill(r, vx(x), vx(e), W);
      }
    }
  }
}

// A line in the card's text font, moved from centred to the box's left.
static void lineLeft(TextOverlay *o, const char *s, const TextBox &b) {
  const int bx0 = vx(b.x0);
  textPrepare(o, s, bx0, vx(b.x1), vy(b.y0), vy(b.y1));
  if (o->count <= 0) return;
  int left = o->x[0];
  for (int i = 1; i < o->count; i++) left = o->x[i] < left ? o->x[i] : left;
  for (int i = 0; i < o->count; i++) o->x[i] = (int16_t)(o->x[i] - (left - bx0));
}

// The light half of the init: the call laid out and scaled into its
// coverage, the two lines prepared.
static void bbcmicroReflow() {
  if (!letters) return;
  Lettering &L = *letters;
  const int bx0 = vx(CALL_BOX.x0), bx1 = vx(CALL_BOX.x1);
  const int by0 = vy(CALL_BOX.y0), by1 = vy(CALL_BOX.y1);
  interInkBuild(&L.ink, textInkId());
  L.cov = freeMem;
  interLineLayout(cfgTextIdShown(), bx1 - bx0, by1 - by0, freeMem, scratchEnd, &L.w, &L.h,
                  CALL_SQUEEZE);
  L.x = bx0 + (bx1 - bx0 - L.w) / 2;
  L.y = by0 + (by1 - by0 - L.h) / 2;
  lineLeft(&L.line[0], cfgTextSubShown(), LINE_BOX[0]);
  lineLeft(&L.line[1], cfgLegacyText(), LINE_BOX[1]);
}

// A colour change on the card in view refreshes the call's ink, once a
// picture.
static void bbcmicroTick() {
  if (!letters) return;
  const YCbCr now = textInkId(), was = letters->ink.ink;
  if (now.Y != was.Y || now.Cb != was.Cb || now.Cr != was.Cr) interInkBuild(&letters->ink, now);
}

static void bbcmicroInit() {
  uint32_t bytes = 0;
  uint8_t *scratch = romBorrowScratch(&bytes);
  const uint32_t head = SCREEN_H + sizeof(Lettering);
  if (bytes < head + 2 * ROW_BYTES) {
    rows = nullptr;
    letters = nullptr;
    return;
  }
  letters = (Lettering *)scratch;
  rowOf = scratch + sizeof(Lettering);
  rows = (uint8_t *)(((uintptr_t)(rowOf + SCREEN_H) + 3) & ~(uintptr_t)3);
  // Room for the distinct rows (18 with this layout) plus the work row;
  // the call's coverage goes after them.
  const int maxRows = 48;
  scratchEnd = scratch + bytes;
  if (rows + maxRows * ROW_BYTES > scratchEnd) {
    rows = nullptr;
    letters = nullptr;
    return;
  }

  // Compose every row once; keep one copy of each distinct row. The last
  // slot is the work row.
  int n = 0;
  uint8_t *work = rows + (maxRows - 1) * ROW_BYTES;
  for (int y = 0; y < SCREEN_H; y++) {
    composeRow(y, work);
    int k = n - 1;
    if (k >= 0 && memcmp(rows + k * ROW_BYTES, work, ROW_BYTES) == 0) {
      rowOf[y] = (uint8_t)k;       // the same as the row above: by far the most common
      continue;
    }
    for (k = 0; k < n; k++) {
      if (memcmp(rows + k * ROW_BYTES, work, ROW_BYTES) == 0) break;
    }
    if (k == n && n < maxRows - 1 && n < 255) memcpy(rows + n++ * ROW_BYTES, work, ROW_BYTES);
    rowOf[y] = (uint8_t)(k < n ? k : n - 1);
  }
  freeMem = rows + maxRows * ROW_BYTES;
  bbcmicroReflow();
}

static void bbcmicroRenderRow(uint8_t *base, int y) {
  if (!rows) {
    uint32_t *w = (uint32_t *)base;
    for (int i = 0; i < SCREEN_W / 2; i++) w[i] = 0x10801080u;
    return;
  }
  memcpy(base, rows + rowOf[y] * ROW_BYTES, ROW_BYTES);
  const Lettering &L = *letters;
  const int r = y - L.y;
  if (r >= 0 && r < L.h) interDrawRow(base, L.x, L.cov + r * L.w, L.w, &L.ink);
  const YCbCr sub = textInkSub();
  textDrawRow(base, &L.line[0], y, sub);
  textDrawRow(base, &L.line[1], y, sub);
}

const Pattern PATTERN_BBCMICRO = {LANG_CARD_BBCMICRO, bbcmicroInit, bbcmicroRenderRow, bbcmicroTick,
                                  false, false, bbcmicroReflow};

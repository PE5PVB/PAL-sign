// Contest page: a black picture with large white text, a line at the
// top, a line at the bottom, and a four digit counter in the middle.
// Meant to keep a callsign, locator and serial number on screen during
// a contest. All of it is settable over the serial port and saved; see
// settings.h.
//
// mono = true: no colour in this picture, so the encoder switches
// chroma and burst off (register 0x84, bits 4 and 5).
//
// The two text lines are Inter; the counter digits are a separate
// face, Titillium Web Bold. Both come pre-rendered at final size from
// tools/make_contest_digits.py / tools/make_contest_font.py, stored as
// RLE runs rather than a coverage byte per pixel: the row budget does
// not allow the alternative in a band this deep (148 rows). See
// tools/contest_rle.py.
//
// A text line wider than its box falls back to the card font via
// textPrepareScaled(), which comes down in scale until it fits; the
// scale from settings is a maximum, not a fixed size.
#include <stdio.h>

#include "../gfx.h"
#include "../language.h"
#include "../pattern.h"
#include "../settings.h"
#include "../rletext.h"
#include "../textoverlay.h"
#include "contest_digits.h"
#include "contest_font.h"
#include "contest_face.h"

// The three boxes, spread over 576 rows so a line at max scale can't
// run into the next one. 20 px margin either side for overscan.
static const int BOX_X0 = 20, BOX_X1 = 700;
static const int T1_Y0 = 40, T1_Y1 = 150;
static const int NR_Y0 = 200, NR_Y1 = 376;
static const int T2_Y0 = 426, T2_Y1 = 536;

// A text line in the outline face, laid out once at init. Lower case
// folds to upper case and the slashed O arrives as 0x01, same rules as
// textPrepareScaled.
static const int LINE_MAX = 32;   // the stored texts are 31 characters at most

struct ContestLine {
  const ContestGlyph *g[LINE_MAX];
  int16_t x[LINE_MAX];
  uint8_t n;
  int16_t yCap;         // picture row of the capital top
  bool outline;         // false: the raster fallback below is in use
  TextOverlay fallback;
};
static ContestLine tLine1, tLine2;

// Coverage to luma: 0 stays black level 16, 255 is white level 235.
static uint8_t contestLut[256];

static void contestFillLut() {
  for (int i = 0; i < 256; i++) contestLut[i] = (uint8_t)(16 + (219 * i) / 255);
}

// EXPORTED: Mixed bars letters its own line in this same face at this
// same size (see mixedbar.cpp) and reuses this instead of a second
// #include of contest_font.h, which would double the tokens in flash.
const ContestGlyph *contestGlyphFor(const char **pp) {
  const char *p = *pp;
  const uint8_t b = (uint8_t)*p;
  char c;
  if (b == 0xC3 && ((uint8_t)p[1] == 0x98 || (uint8_t)p[1] == 0xB8)) {
    c = 0x01;   // slashed O in UTF-8
    *pp = p + 1;
  } else if (b == 0xD8 || b == 0xF8) {
    c = 0x01;   // slashed O in Latin-1
  } else if (b >= 'a' && b <= 'z') {
    c = (char)(b - 'a' + 'A');
  } else {
    c = (char)b;
  }
  const int n = (int)(sizeof(CONTEST_FONT) / sizeof(CONTEST_FONT[0]));
  for (int i = 0; i < n; i++) {
    if (CONTEST_FONT[i].c == (uint8_t)c) return &CONTEST_FONT[i];
  }
  return &CONTEST_FONT[0];   // unknown character -> space
}

int contestFaceTop() { return CONTEST_FONT_TOP; }
int contestFaceBottom() { return CONTEST_FONT_BOTTOM; }

static void contestPrepareLine(ContestLine *L, const char *s, int by0, int by1) {
  const ContestGlyph *gs[LINE_MAX];
  int n = 0;
  for (const char *p = s; *p && n < LINE_MAX; p++) gs[n++] = contestGlyphFor(&p);
  L->n = 0;
  L->outline = true;
  if (n == 0) return;

  int total = 0;
  for (int i = 0; i < n; i++) total += gs[i]->adv;
  const int spaceW = (BOX_X1 - BOX_X0) - 6;   // the margin the raster path keeps
  if (total > spaceW) {
    L->outline = false;
    textPrepareScaled(&L->fallback, s, BOX_X0, BOX_X1, by0, by1, cfgContestScaleText());
    return;
  }

  // Centred on the real ink, vertically too: a line of flat capitals
  // must not hang low just because the face keeps room for tails.
  int top = 32767, bot = -32768;
  for (int i = 0; i < n; i++) {
    if (!gs[i]->h) continue;
    if (gs[i]->yoff < top) top = gs[i]->yoff;
    if (gs[i]->yoff + gs[i]->h > bot) bot = gs[i]->yoff + gs[i]->h;
  }
  if (bot <= top) { top = 0; bot = CONTEST_FONT_CAP; }
  L->yCap = (int16_t)(by0 + ((by1 - by0) - (bot - top)) / 2 - top);

  int pen = BOX_X0 + ((BOX_X1 - BOX_X0) - total) / 2;
  for (int i = 0; i < n; i++) {
    L->g[i] = gs[i];
    L->x[i] = (int16_t)(pen + gs[i]->lsb);
    pen += gs[i]->adv;
  }
  L->n = (uint8_t)n;
}

static void contestDrawLineRow(uint8_t *base, ContestLine *L, int y) {
  if (!L->outline) {
    textDrawRow(base, &L->fallback, y, GFX_WHITE);
    return;
  }
  for (int i = 0; i < L->n; i++) {
    const ContestGlyph *g = L->g[i];
    if (!g->h) continue;
    const int r = y - (L->yCap + g->yoff);
    if (r < 0 || r >= g->h) continue;
    rleBlendRow(base, L->x[i], g->rle + g->off[r], g->rle + g->off[r + 1], contestLut,
                235, 128, 128, SCREEN_W);
  }
}

// The counter, laid out once at init: four cells of the widest digit,
// centred in the number box, each digit centred in its own cell.
static char numText[5];
static int numX[4];
static int numY0;

static void contestInit() {
  contestFillLut();
  snprintf(numText, sizeof(numText), "%04d", cfgContestNumber());

  const int gap = 20;
  const int total = 4 * CONTEST_DIGIT_CELL + 3 * gap;
  int x = BOX_X0 + ((BOX_X1 - BOX_X0) - total) / 2;
  for (int i = 0; i < 4; i++) {
    numX[i] = x;
    x += CONTEST_DIGIT_CELL + gap;
  }
  numY0 = NR_Y0 + ((NR_Y1 - NR_Y0) - CONTEST_DIGIT_H) / 2;

  contestPrepareLine(&tLine1, cfgContestText1(), T1_Y0, T1_Y1);
  contestPrepareLine(&tLine2, cfgContestText2(), T2_Y0, T2_Y1);
}

static void contestDrawNumberRow(uint8_t *base, int y) {
  if (y < numY0 || y >= numY0 + CONTEST_DIGIT_H) return;
  const int row = y - numY0;
  for (int i = 0; i < 4; i++) {
    const char c = numText[i];
    if (c < '0' || c > '9') continue;
    const ContestDigit *d = &CONTEST_DIGITS[c - '0'];
    const int x0 = numX[i] + (CONTEST_DIGIT_CELL - d->w) / 2;
    rleBlendRow(base, x0, d->rle + d->off[row], d->rle + d->off[row + 1], contestLut,
                235, 128, 128, SCREEN_W);
  }
}

static void contestRenderRow(uint8_t *base, int y) {
  uint32_t *w = (uint32_t *)base;
  for (int i = 0; i < SCREEN_W / 2; i++) *w++ = 0x10801080u;   // Cb,Y,Cr,Y: neutral chroma, black luma

  contestDrawLineRow(base, &tLine1, y);
  contestDrawNumberRow(base, y);
  contestDrawLineRow(base, &tLine2, y);
}

const Pattern PATTERN_CONTEST = {LANG_CARD_ATV_CONTEST, contestInit, contestRenderRow, nullptr, true,
                                 false};

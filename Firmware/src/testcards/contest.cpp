// Contest pages: a black picture with large white text, a line at the
// top, a four digit counter under it, a second line, and a frequency
// line at the bottom. Meant to keep a callsign, serial number, locator
// and working frequency on screen during a contest. Four of them, each
// with its own texts and number, so a band change is a card change;
// all of it is settable over the serial port and saved, see settings.h.
// The page number is baked into each card via CONTEST_CARD(n) below,
// the same way custom.cpp does it for the photo slots.
//
// mono = true: no colour in this picture, so the encoder switches
// chroma and burst off (register 0x84, bits 4 and 5).
//
// The text lines are Inter, the frequency line in a smaller cut of
// it (60 row capitals against 90) so a frequency with decimals and
// unit fits the box; the counter digits are a separate face, Titillium
// Web Bold. All come pre-rendered at final size from
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
#include "contest_freq_font.h"
#include "contest_face.h"

// The four boxes over 576 rows: 100 rows per text line (capitals are
// 90, the ink is centred), the digit band exactly its 148, 70 rows
// for the frequency line (capitals 60), 24 rows between them and 40
// top, 46 bottom for overscan. 20 px margin either side for the same
// reason.
static const int BOX_X0 = 20, BOX_X1 = 700;
static const int T1_Y0 = 40, T1_Y1 = 140;
static const int NR_Y0 = 164, NR_Y1 = 312;
static const int T2_Y0 = 336, T2_Y1 = 436;
static const int T3_Y0 = 460, T3_Y1 = 530;

// A text line in the outline face, laid out once at init. Lower case
// folds to upper case and the slashed O arrives as 0x01, same rules as
// textPrepareScaled.
static const int LINE_MAX = 32;   // the stored texts are 31 characters at most

// A line is either outline glyphs or the raster fallback, never both,
// so the two share their storage: the main RAM region is full to the
// last few hundred bytes.
struct ContestLine {
  uint8_t n;
  int16_t yCap;         // picture row of the capital top
  bool outline;         // false: the raster fallback below is in use
  union {
    struct {
      const ContestGlyph *g[LINE_MAX];
      int16_t x[LINE_MAX];
    } o;
    TextOverlay fallback;
  };
};
static ContestLine tLine1, tLine2, tLine3;

// Coverage to luma: 0 stays black level 16, 255 is white level 235.
static uint8_t contestLut[256];

static void contestFillLut() {
  for (int i = 0; i < 256; i++) contestLut[i] = (uint8_t)(16 + (219 * i) / 255);
}

static const ContestGlyph *glyphIn(const ContestGlyph *tbl, int n, const char **pp) {
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
  for (int i = 0; i < n; i++) {
    if (tbl[i].c == (uint8_t)c) return &tbl[i];
  }
  return &tbl[0];   // unknown character -> space
}

// EXPORTED: Mixed bars letters its own line in this same face at this
// same size (see mixedbar.cpp) and reuses this instead of a second
// #include of contest_font.h, which would double the tokens in flash.
const ContestGlyph *contestGlyphFor(const char **pp) {
  return glyphIn(CONTEST_FONT, (int)(sizeof(CONTEST_FONT) / sizeof(CONTEST_FONT[0])), pp);
}

static const ContestGlyph *freqGlyphFor(const char **pp) {
  return glyphIn(CONTEST_FREQ_FONT,
                 (int)(sizeof(CONTEST_FREQ_FONT) / sizeof(CONTEST_FREQ_FONT[0])), pp);
}

int contestFaceTop() { return CONTEST_FONT_TOP; }
int contestFaceBottom() { return CONTEST_FONT_BOTTOM; }

static void contestPrepareLine(ContestLine *L, const char *s, int by0, int by1,
                               const ContestGlyph *(*lookup)(const char **), int cap) {
  const ContestGlyph *gs[LINE_MAX];
  int n = 0;
  for (const char *p = s; *p && n < LINE_MAX; p++) gs[n++] = lookup(&p);
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
  if (bot <= top) { top = 0; bot = cap; }
  L->yCap = (int16_t)(by0 + ((by1 - by0) - (bot - top)) / 2 - top);

  int pen = BOX_X0 + ((BOX_X1 - BOX_X0) - total) / 2;
  for (int i = 0; i < n; i++) {
    L->o.g[i] = gs[i];
    L->o.x[i] = (int16_t)(pen + gs[i]->lsb);
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
    const ContestGlyph *g = L->o.g[i];
    if (!g->h) continue;
    const int r = y - (L->yCap + g->yoff);
    if (r < 0 || r >= g->h) continue;
    rleBlendRow(base, L->o.x[i], g->rle + g->off[r], g->rle + g->off[r + 1], contestLut,
                235, 128, 128, SCREEN_W, false);   // chroma is 128 already: the page is neutral
  }
}

// The counter, laid out once at init: four cells of the widest digit,
// centred in the number box, each digit centred in its own cell.
static char numText[5];
static int numX[4];
static int numY0;
static int activePage = 0;

static void contestInit(int page) {
  activePage = page;
  contestFillLut();
  snprintf(numText, sizeof(numText), "%04d", cfgContestNumber(page));

  const int gap = 20;
  const int total = 4 * CONTEST_DIGIT_CELL + 3 * gap;
  int x = BOX_X0 + ((BOX_X1 - BOX_X0) - total) / 2;
  for (int i = 0; i < 4; i++) {
    numX[i] = x;
    x += CONTEST_DIGIT_CELL + gap;
  }
  numY0 = NR_Y0 + ((NR_Y1 - NR_Y0) - CONTEST_DIGIT_H) / 2;

  contestPrepareLine(&tLine1, cfgContestText1(page), T1_Y0, T1_Y1, contestGlyphFor,
                     CONTEST_FONT_CAP);
  contestPrepareLine(&tLine2, cfgContestText2(page), T2_Y0, T2_Y1, contestGlyphFor,
                     CONTEST_FONT_CAP);
  contestPrepareLine(&tLine3, cfgContestText3(page), T3_Y0, T3_Y1, freqGlyphFor,
                     CONTEST_FREQ_FONT_CAP);
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
                235, 128, 128, SCREEN_W, false);
  }
}

static void contestRenderRow(uint8_t *base, int y) {
  uint32_t *w = (uint32_t *)base;
  for (int i = 0; i < SCREEN_W / 2; i++) *w++ = 0x10801080u;   // Cb,Y,Cr,Y: neutral chroma, black luma

  contestDrawLineRow(base, &tLine1, y);
  contestDrawNumberRow(base, y);
  contestDrawLineRow(base, &tLine2, y);
  contestDrawLineRow(base, &tLine3, y);
}

// One init function and one Pattern per page, built by the
// preprocessor so the page a card shows can never drift from its name.
#define CONTEST_CARD(n, name)                                                   \
  static void contest##n##Init() { contestInit((n) - 1); }                      \
  const Pattern PATTERN_CONTEST##n = {name, contest##n##Init, contestRenderRow, \
                                      nullptr, true, false}

CONTEST_CARD(1, LANG_CARD_ATV_CONTEST_1);
CONTEST_CARD(2, LANG_CARD_ATV_CONTEST_2);
CONTEST_CARD(3, LANG_CARD_ATV_CONTEST_3);
CONTEST_CARD(4, LANG_CARD_ATV_CONTEST_4);

static const Pattern *const CONTEST_CARDS[CONTEST_PAGES] = {
    &PATTERN_CONTEST1, &PATTERN_CONTEST2, &PATTERN_CONTEST3, &PATTERN_CONTEST4};

int contestPageOf(const Pattern *p) {
  for (int i = 0; i < CONTEST_PAGES; i++) {
    if (CONTEST_CARDS[i] == p) return i;
  }
  return -1;
}

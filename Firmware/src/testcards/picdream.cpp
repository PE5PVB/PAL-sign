// Picdream: colour bars, a black box for the first line (row 144..217,
// x 180..539, between the yellow and blue bars), colour bars again,
// then a multiburst band (row 432..503) reusing multiburst.cpp's exact
// signal, and a last strip of bars. All boundaries even, so none
// flickers between fields.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "../gfx.h"
#include "../language.h"
#include "../pattern.h"
#include "../rletext.h"
#include "../settings.h"
#include "../textoverlay.h"
#include "picdream_font.h"

static const int BAR_COUNT = 8;
static const int BAR_WORDS = SCREEN_W / 2 / BAR_COUNT;

// Bar boundaries, 90 samples apart: white 0, yellow 90, cyan 180,
// green 270, magenta 360, red 450, blue 540, black 630.
static const int TEXT_X0 = 180;
static const int TEXT_X1 = 540;

static const int TEXT_Y0 = SCREEN_H / 4;
static const int TEXT_PAD = 4;
static int TEXT_Y1;   // set at init from the font's own band

static const int MULTIBURST_Y0 = SCREEN_H * 3 / 4;
static const int MULTIBURST_Y1 = SCREEN_H * 7 / 8;

static uint32_t colourWord[BAR_COUNT];

// The multiburst band, built once at init and replayed per row.
static uint8_t multiburstRow[SCREEN_W * 2];

// One sample of tools/convert_rom_pattern.py's build_frame_multiburst()
// signal (same card multiburst.cpp shows), computed one sample at a
// time rather than into a float[SCREEN_W] buffer first: that would put
// nearly 3kB on the stack at every card change, on a project already
// tight on RAM.
static float multiburstSample(int x) {
  static const float MHZ[6] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 4.8f};
  const float fs = 13.5e6f;
  const float amp = 66.0f;
  const int width = 96, gap = 4, x0 = 112;
  if (x < 48) return 16.0f;
  if (x < 96) return 235.0f;
  if (x >= x0) {
    const int rel = x - x0;
    const int step = width + gap;
    const int k = rel / step;
    const int n = rel % step;
    if (k < 6 && n < width) {
      return 126.0f + amp * sinf(2.0f * (float)M_PI * MHZ[k] * 1.0e6f * (float)n / fs);
    }
  }
  return 126.0f;
}

static void buildMultiburstRow() {
  for (int x = 0; x < SCREEN_W; x++) {
    int v = (int)(multiburstSample(x) + 0.5f);
    if (v < 16) v = 16;
    if (v > 235) v = 235;
    multiburstRow[(x >> 1) * 4 + 1 + (x & 1) * 2] = (uint8_t)v;
  }
  for (int p = 0; p < SCREEN_W / 2; p++) {
    multiburstRow[p * 4 + 0] = 128;   // Cb, monochrome
    multiburstRow[p * 4 + 2] = 128;   // Cr, monochrome
  }
}

static const int LINE_MAX = 32;
static const ContestGlyph *lineG[LINE_MAX];
static int16_t lineX[LINE_MAX];
static int lineN;
static int capRow;
static bool big;      // false: the raster fallback is up
static bool textOn;
static TextOverlay fallback;

static YCbCr lineInk;
static uint8_t lineLut[256];

static void buildLut() {
  lineInk = textInkId();
  for (int i = 0; i < 256; i++) {
    lineLut[i] = (uint8_t)(16 + ((lineInk.Y - 16) * i) / 255);
  }
}

// Folds a character like contestGlyphFor(), indexing PICDREAM_FONT
// instead: a table of its own, used by no other card. The font is 66
// rows, not the contest page's 90 -- a typical callsign does not fit
// the 360-sample box at 90.
static const ContestGlyph *picdreamGlyphFor(const char **pp) {
  const char *p = *pp;
  const uint8_t b = (uint8_t)*p;
  char c;
  if (b == 0xC3 && ((uint8_t)p[1] == 0x98 || (uint8_t)p[1] == 0xB8)) {
    c = 0x01;
    *pp = p + 1;
  } else if (b == 0xD8 || b == 0xF8) {
    c = 0x01;
  } else if (b >= 'a' && b <= 'z') {
    c = (char)(b - 'a' + 'A');
  } else {
    c = (char)b;
  }
  const int n = (int)(sizeof(PICDREAM_FONT) / sizeof(PICDREAM_FONT[0]));
  for (int i = 0; i < n; i++) {
    if (PICDREAM_FONT[i].c == (uint8_t)c) return &PICDREAM_FONT[i];
  }
  return &PICDREAM_FONT[0];
}

// The light half of the init: only the text layout. See pattern.h.
static void picdreamReflow() {
  buildLut();
  // Called unconditionally, switch on or off; see mixedbar.cpp's
  // identical note.
  const char *s = cfgTextIdShown();
  textOn = cfgShowId();
  if (!textOn) return;
  const ContestGlyph *gs[LINE_MAX];
  int n = 0;
  int width = 0;
  for (const char *p = s; *p && n < LINE_MAX; p++) {
    gs[n] = picdreamGlyphFor(&p);
    width += gs[n]->adv;
    n++;
  }

  // Bounded to the box (360 samples), not the full screen.
  const int spaceW = (TEXT_X1 - TEXT_X0) - 12;
  if (width > spaceW) {
    big = false;
    const int keep = textGetFont();
    textSetFont(2);   // FONTS[2] is Inter
    textPrepareScaled(&fallback, s, TEXT_X0, TEXT_X1, TEXT_Y0, TEXT_Y1, 1);
    textSetFont(keep);
    return;
  }

  big = true;
  lineN = n;
  int pen = TEXT_X0 + ((TEXT_X1 - TEXT_X0) - width) / 2;
  for (int i = 0; i < n; i++) {
    lineG[i] = gs[i];
    lineX[i] = (int16_t)pen;
    pen += gs[i]->adv;
  }

  // Centred on this line's own ink, not the face's band; see
  // mixedbar.cpp's identical rule.
  int top = 32767, bot = -32768;
  for (int i = 0; i < n; i++) {
    if (!gs[i]->h) continue;
    if (gs[i]->yoff < top) top = gs[i]->yoff;
    if (gs[i]->yoff + gs[i]->h > bot) bot = gs[i]->yoff + gs[i]->h;
  }
  if (bot <= top) { top = 0; bot = 0; }
  capRow = TEXT_Y0 + ((TEXT_Y1 - TEXT_Y0) - (bot - top)) / 2 - top;
}

static void picdreamInit() {
  static const uint32_t RGB[BAR_COUNT] = {0xFFFFFF, 0xBFBF00, 0x00BFBF, 0x00BF00,
                                          0xBF00BF, 0xBF0000, 0x0000BF, 0x000000};
  for (int i = 0; i < BAR_COUNT; i++) {
    const YCbCr c = yccFromRgb(RGB[i]);
    colourWord[i] =
        (uint32_t)c.Cb | ((uint32_t)c.Y << 8) | ((uint32_t)c.Cr << 16) | ((uint32_t)c.Y << 24);
  }
  buildMultiburstRow();
  const int band = (PICDREAM_FONT_BOTTOM - PICDREAM_FONT_TOP + 2 * TEXT_PAD + 1) & ~1;
  TEXT_Y1 = TEXT_Y0 + band;
  picdreamReflow();
}

static void picdreamTick() {
  const YCbCr now = textInkId();
  if (now.Y != lineInk.Y || now.Cb != lineInk.Cb || now.Cr != lineInk.Cr) buildLut();
}

static void picdreamRenderRow(uint8_t *base, int y) {
  if (y >= MULTIBURST_Y0 && y < MULTIBURST_Y1) {
    memcpy(base, multiburstRow, SCREEN_W * 2);
    return;
  }

  uint32_t *w = (uint32_t *)base;
  for (int b = 0; b < BAR_COUNT; b++) {
    for (int i = 0; i < BAR_WORDS; i++) *w++ = colourWord[b];
  }

  if (textOn && y >= TEXT_Y0 && y < TEXT_Y1) {
    uint32_t *tw = (uint32_t *)base + TEXT_X0 / 2;
    for (int i = 0; i < (TEXT_X1 - TEXT_X0) / 2; i++) *tw++ = 0x10801080u;
    if (!big) {
      textDrawRow(base, &fallback, y, lineInk);
      return;
    }
    for (int i = 0; i < lineN; i++) {
      const ContestGlyph *g = lineG[i];
      if (!g->h) continue;
      const int r = y - (capRow + g->yoff);
      if (r < 0 || r >= g->h) continue;
      rleBlendRow(base, lineX[i] + g->lsb, g->rle + g->off[r], g->rle + g->off[r + 1],
                  lineLut, lineInk.Y, lineInk.Cb, lineInk.Cr, SCREEN_W);
    }
  }
}

const Pattern PATTERN_PICDREAM = {LANG_CARD_PICDREAM, picdreamInit, picdreamRenderRow, picdreamTick,
                                  false, false, picdreamReflow};

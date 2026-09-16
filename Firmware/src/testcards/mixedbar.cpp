// Mixed bars: colour bars (top half), a black band with the first line
// from row 288 (screen middle) down, then the same bars without chroma.
// The boundary at row 288 is fixed regardless of the text; with the
// first line off, the black/white bars start there directly instead of
// colour bars meeting an empty box.
//
// Colours are the 75% ITU-R BT.471-1 bars, white at 100%, same eight as
// "EBU bars and red"; converted with yccFromRgb() like everywhere else.
// 90 samples per bar (8 x 90 = 720) keeps every boundary on an even
// sample so no chroma pair straddles two bars.
//
// The first line uses CONTEST_FONT (Inter, 90 rows) via
// testcards/contest_face.h regardless of the card's text font setting,
// falling back to the raster Inter through textPrepareScaled() if the
// line is wider than the screen (see textoverlay.cpp's "FOLLOWING
// EDGES" for that engine).
#include "../gfx.h"
#include "../language.h"
#include "../pattern.h"
#include "../rletext.h"
#include "../settings.h"
#include "../textoverlay.h"
#include "contest_face.h"

#include <stdint.h>
#include <string.h>

static const int MID_Y = SCREEN_H / 2;
static const int TEXT_Y0 = MID_Y;
static const int TEXT_PAD = 4;   // air above/below the ink band

// Set at init from contestFaceTop()/contestFaceBottom(): CONTEST_FONT's
// ink band plus 2 x TEXT_PAD, rounded up to even.
static int TEXT_Y1;
static int GREY_Y0;

static const int BAR_COUNT = 8;
static const int BAR_WORDS = SCREEN_W / 2 / BAR_COUNT;

static const int LINE_MAX = 32;
static const ContestGlyph *lineG[LINE_MAX];
static int16_t lineX[LINE_MAX];
static int lineN;
static int capRow;
static bool big;      // false: the raster fallback is up
static bool textOn;
static TextOverlay fallback;

static uint32_t colourWord[BAR_COUNT];
static uint32_t greyWord[BAR_COUNT];

static YCbCr lineInk;
static uint8_t lineLut[256];

static void buildLut() {
  lineInk = textInkId();
  for (int i = 0; i < 256; i++) {
    lineLut[i] = (uint8_t)(16 + ((lineInk.Y - 16) * i) / 255);
  }
}

// The light half of the init: only the text layout. See pattern.h.
static void mixedbarReflow() {
  buildLut();
  // Called unconditionally (switch on or off): this is the only place
  // that tells cfgSetActiveCard() the card has a first-line slot at
  // all, so skipping it with the switch off would hide the PC tool's
  // switch to turn it back on. textOn, not this call, decides the band.
  const char *s = cfgTextIdShown();
  textOn = cfgShowId();
  if (!textOn) return;
  const ContestGlyph *gs[LINE_MAX];
  int n = 0;
  int width = 0;
  for (const char *p = s; *p && n < LINE_MAX; p++) {
    gs[n] = contestGlyphFor(&p);
    width += gs[n]->adv;
    n++;
  }

  const int spaceW = SCREEN_W - 12;
  if (width > spaceW) {
    big = false;
    const int keep = textGetFont();
    textSetFont(2);   // FONTS[2] is Inter
    textPrepareScaled(&fallback, s, 0, SCREEN_W, TEXT_Y0, TEXT_Y1, 3);
    textSetFont(keep);
    return;
  }

  big = true;
  lineN = n;
  int pen = (SCREEN_W - width) / 2;
  for (int i = 0; i < n; i++) {
    lineG[i] = gs[i];
    lineX[i] = (int16_t)pen;
    pen += gs[i]->adv;
  }

  // Centred on this line's own ink, not the face's band: flat capitals
  // must not hang low just because the face keeps room for tails (Q, J).
  int top = 32767, bot = -32768;
  for (int i = 0; i < n; i++) {
    if (!gs[i]->h) continue;
    if (gs[i]->yoff < top) top = gs[i]->yoff;
    if (gs[i]->yoff + gs[i]->h > bot) bot = gs[i]->yoff + gs[i]->h;
  }
  if (bot <= top) { top = 0; bot = 0; }
  capRow = TEXT_Y0 + ((TEXT_Y1 - TEXT_Y0) - (bot - top)) / 2 - top;
}

static void mixedbarInit() {
  static const uint32_t RGB[BAR_COUNT] = {0xFFFFFF, 0xBFBF00, 0x00BFBF, 0x00BF00,
                                          0xBF00BF, 0xBF0000, 0x0000BF, 0x000000};
  for (int i = 0; i < BAR_COUNT; i++) {
    const YCbCr c = yccFromRgb(RGB[i]);
    colourWord[i] =
        (uint32_t)c.Cb | ((uint32_t)c.Y << 8) | ((uint32_t)c.Cr << 16) | ((uint32_t)c.Y << 24);
    greyWord[i] = 128u | ((uint32_t)c.Y << 8) | (128u << 16) | ((uint32_t)c.Y << 24);
  }
  const int band = (contestFaceBottom() - contestFaceTop() + 2 * TEXT_PAD + 1) & ~1;
  TEXT_Y1 = MID_Y + band;
  GREY_Y0 = TEXT_Y1;
  mixedbarReflow();
}

// A colour change on the card in view refreshes the ink cache, once per
// picture.
static void mixedbarTick() {
  const YCbCr now = textInkId();
  if (now.Y != lineInk.Y || now.Cb != lineInk.Cb || now.Cr != lineInk.Cr) buildLut();
}

static void mixedbarRenderRow(uint8_t *base, int y) {
  uint32_t *w = (uint32_t *)base;
  if (y < TEXT_Y0) {
    for (int b = 0; b < BAR_COUNT; b++) {
      for (int i = 0; i < BAR_WORDS; i++) *w++ = colourWord[b];
    }
  } else if (textOn && y < GREY_Y0) {
    for (int i = 0; i < SCREEN_W / 2; i++) *w++ = 0x10801080u;  // Cb,Y,Cr,Y: black
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
  } else {
    for (int b = 0; b < BAR_COUNT; b++) {
      for (int i = 0; i < BAR_WORDS; i++) *w++ = greyWord[b];
    }
  }
}

const Pattern PATTERN_MIXEDBAR = {LANG_CARD_MIXED_BARS, mixedbarInit, mixedbarRenderRow, mixedbarTick,
                                  false, false, mixedbarReflow};

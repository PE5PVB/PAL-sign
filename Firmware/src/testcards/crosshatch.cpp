// Crosshatch: the same grid as card 1 (PM5644 G00), redrawn as white
// lines on a plain black field, colour burst off. The HLINES/VLINES
// positions below were measured from card 1's real decoded picture,
// not designed. Not reproduced: card 1's coloured checkerboard, its
// ring and its needle.
//
// renderRow() does no per-pixel work: a line row is a flat white fill,
// and every other row is identical (the vertical lines don't depend on
// y), so that row is built once at init into vlineRow[] with
// fillSpanSolid() (some line positions are odd, so a line can straddle
// two Cb,Y,Cr,Y words) and replayed per picture with memcpy.
#include <string.h>

#include "../gfx.h"
#include "../language.h"
#include "../pattern.h"

static const int LINE_T = 2;  // every line, row or column, this many samples wide

static const int HLINES[] = {13, 55, 97, 139, 181, 223, 265, 307, 349, 391, 433, 475, 517, 559};
static const int VLINES[] = {32,  70,  109, 147, 186, 224, 263, 301, 340,
                             378, 416, 455, 493, 532, 570, 609, 647, 686};

static uint8_t vlineRow[SCREEN_W * 2];  // one full Cb,Y,Cr,Y row, built once

static bool isHLine(int y) {
  for (int i = 0; i < (int)(sizeof(HLINES) / sizeof(HLINES[0])); i++) {
    if (y >= HLINES[i] && y < HLINES[i] + LINE_T) return true;
  }
  return false;
}

static void crosshatchInit() {
  uint32_t *w = (uint32_t *)vlineRow;
  for (int i = 0; i < SCREEN_W / 2; i++) *w++ = 0x10801080u;  // GFX_BLACK, packed
  for (int i = 0; i < (int)(sizeof(VLINES) / sizeof(VLINES[0])); i++) {
    fillSpanSolid(vlineRow, VLINES[i], VLINES[i] + LINE_T, GFX_WHITE);
  }
}

static void crosshatchRenderRow(uint8_t *base, int y) {
  if (!isHLine(y)) {
    memcpy(base, vlineRow, sizeof(vlineRow));
    return;
  }
  uint32_t *w = (uint32_t *)base;
  for (int i = 0; i < SCREEN_W / 2; i++) *w++ = 0xEB80EB80u;  // GFX_WHITE, packed
}

// mono = true: no colour anywhere in this pattern, so the encoder
// leaves chroma and the colour burst off, as asked.
const Pattern PATTERN_CROSSHATCH = {LANG_CARD_CROSSHATCH, crosshatchInit, crosshatchRenderRow, nullptr,
                                    true, false};

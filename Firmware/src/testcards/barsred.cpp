// EBU colour bars over a red field: the bars check the colour decoder,
// the red field shows ringing/noise/compression on a flat area. Drawn
// by tools/make_barsred.py.
//
//   row   0..371  eight equal colour bars, 90 samples each
//   row 372..575  a plain red field, the same red as the sixth bar
//
// 75 percent colour bars (ITU-R BT.471-1), white at 100 percent, falling
// luminance order: white, yellow, cyan, green, magenta, red, blue, black.
// Both boundaries are even rows/samples: interlace-safe, and no chroma
// pair straddles two bars. Two unique rows, so 1 kB of flash.
#include "barsred_data.h"
#include "../gfx.h"
#include "../language.h"
#include "../pattern.h"
#include "../rompattern.h"
#include "../settings.h"
#include "../textoverlay.h"

static_assert(BARSRED_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(BARSRED_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(BARSRED_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

// The two text boxes, one above the other in the red field, the only
// area text can go on this card. 216..504 matches EBU BW's own box, so
// text comes out the same size and centred; the red field runs 372..575
// so the two 60 row boxes sit either side of its middle, row 474.
static const int TEXT_X0 = 216, TEXT_X1 = 504;
static const int TEXT1_Y0 = 414, TEXT1_Y1 = 474;
static const int TEXT2_Y0 = 474, TEXT2_Y1 = 534;

static TextOverlay idLine, subLine;

// The light half of the init: only the text layout. See pattern.h.
static void barsredReflow() {
  textPrepare(&idLine, cfgTextIdShown(), TEXT_X0, TEXT_X1, TEXT1_Y0, TEXT1_Y1);
  textPrepare(&subLine, cfgTextSubShown(), TEXT_X0, TEXT_X1, TEXT2_Y0, TEXT2_Y1);
}

static void barsredInit() {
  RomTables t;
  t.pal = barsred_pal;
  t.idx = barsred_idx;
  t.len = barsred_len;
  t.start = barsred_start;
  t.line = barsred_line;  // only one variant
  t.palSize = BARSRED_PAL_SIZE;
  t.runCount = BARSRED_RUN_COUNT;
  t.uniqueLines = BARSRED_UNIQUE_LINES;
  romLoad(&t);
  barsredReflow();
}

static void barsredRenderRow(uint8_t *base, int y) {
  romRenderRow(base, y);
  // One box behind both lines (TEXT1_Y0..TEXT2_Y1, they're contiguous),
  // drawn only when there's text, so an unused card stays plain red.
  if (idLine.count || subLine.count)
    textFillBox(base, TEXT_X0, TEXT_X1, TEXT1_Y0, TEXT2_Y1, y, GFX_BLACK);
  textDrawRow(base, &idLine, y, textInkId());
  textDrawRow(base, &subLine, y, textInkSub());
}

const Pattern PATTERN_BARSRED = {LANG_CARD_EBU_BARS_RED, barsredInit, barsredRenderRow, nullptr,
                                 false, false, barsredReflow};

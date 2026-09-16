// The FUBK test card, the German counterpart of the PM5544. Comes from
// a composite baseband recording (hacktv test signals), demodulated
// back to YCbCr with tools/paldec.py — four frames average out the PAL
// subcarrier exactly, so the luma needed no filtering.
//
// THE TWO STRIPED FIELDS RIGHT OF THE GRADIENT BARS BELONG THERE: the
// Unbunt fields, which deliberately mis-modulate one colour-difference
// carrier each to check a receiver's PAL decoder/delay line. A working
// decoder averages them down to the grey of the surrounding grid; any
// colour showing there means a fault. Do not smooth this out, and do
// not reorder the picture rows — the row-to-row alternation the delay
// line cancels depends on rows n and n+2 (not n and n+1) pairing up,
// which is what the field split does.
#include "../config.h"
#include "fubk4x3_data.h"
#include "../gfx.h"
#include "../language.h"
#include "../movingline.h"
#include "../pattern.h"
#include "../rompattern.h"
#include "../settings.h"
#include "../textoverlay.h"

static_assert(FUBK4X3_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(FUBK4X3_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(FUBK4X3_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

// The two black boxes of the middle band, from the data itself: black
// on x 230..357, a white divider on 358..362, black again on 363..490.
static const int ID_BOX_X0 = 230, ID_BOX_X1 = 358;
static const int ID_BOX_Y0 = 285, ID_BOX_Y1 = 324;
static const int SUB_BOX_X0 = 363, SUB_BOX_X1 = 491;

static TextOverlay idLine, subLine;

// The light half of the init: only the text layout. See pattern.h.
static void fubk4x3Reflow() {
  textPrepare(&idLine, cfgTextIdShown(), ID_BOX_X0, ID_BOX_X1, ID_BOX_Y0, ID_BOX_Y1);
  textPrepare(&subLine, cfgTextSubShown(), SUB_BOX_X0, SUB_BOX_X1, ID_BOX_Y0, ID_BOX_Y1);
}

// The moving line sweeps both boxes, divider and all; it only raises
// luma, so crossing the white divider is harmless.
static void fubk4x3Tick() {
  movingLineBox(ID_BOX_X0, SUB_BOX_X1, ID_BOX_Y0, ID_BOX_Y1);
}

static void fubk4x3Init() {
  RomTables t;
  t.pal = fubk4x3_pal;
  t.idx = fubk4x3_idx;
  t.len = fubk4x3_len;
  t.start = fubk4x3_start;
  t.line = fubk4x3_line;
  t.palSize = FUBK4X3_PAL_SIZE;
  t.runCount = FUBK4X3_RUN_COUNT;
  t.uniqueLines = FUBK4X3_UNIQUE_LINES;
  romLoad(&t);
  fubk4x3Reflow();
}

static void fubk4x3RenderRow(uint8_t *base, int y) {
  romRenderRow(base, y);
  textDrawRow(base, &idLine, y, textInkId());
  textDrawRow(base, &subLine, y, textInkSub());
  movingLineDrawRow(base, y);   // last, so the stripe stays unbroken over text
}

const Pattern PATTERN_FUBK4X3 = {LANG_CARD_FUBK, fubk4x3Init, fubk4x3RenderRow, fubk4x3Tick, false,
                                 false, fubk4x3Reflow};

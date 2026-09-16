// The FUBK test card in 16:9, anamorphic. Like fubk4x3.cpp this comes
// from a composite baseband recording, demodulated with tools/paldec.py.
// The two striped fields right of the gradient bars are the Unbunt
// fields; see fubk4x3.cpp for what they do and why they must not be
// smoothed out.
#include "../config.h"
#include "fubk16x9_data.h"
#include "../gfx.h"
#include "../language.h"
#include "../movingline.h"
#include "../pattern.h"
#include "../rompattern.h"
#include "../settings.h"
#include "../textoverlay.h"

static_assert(FUBK16X9_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(FUBK16X9_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(FUBK16X9_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

// Delivered horizontally squeezed, so set CFG_ASPECT to 4 (16:9
// anamorphic): the WSS then tells a widescreen set to stretch it.
//
// The two black boxes of the middle band, from the data itself: black
// on x 263..357, a white divider on 358..362, black again on 363..457.
static const int ID_BOX_X0 = 263, ID_BOX_X1 = 358;
static const int ID_BOX_Y0 = 285, ID_BOX_Y1 = 324;
static const int SUB_BOX_X0 = 363, SUB_BOX_X1 = 458;

static TextOverlay idLine, subLine;

// The light half of the init: only the text layout. See pattern.h.
static void fubk16x9Reflow() {
  textPrepare(&idLine, cfgTextIdShown(), ID_BOX_X0, ID_BOX_X1, ID_BOX_Y0, ID_BOX_Y1);
  textPrepare(&subLine, cfgTextSubShown(), SUB_BOX_X0, SUB_BOX_X1, ID_BOX_Y0, ID_BOX_Y1);
}

// The moving line sweeps both boxes, divider and all.
static void fubk16x9Tick() {
  movingLineBox(ID_BOX_X0, SUB_BOX_X1, ID_BOX_Y0, ID_BOX_Y1);
}

static void fubk16x9Init() {
  RomTables t;
  t.pal = fubk16x9_pal;
  t.idx = fubk16x9_idx;
  t.len = fubk16x9_len;
  t.start = fubk16x9_start;
  t.line = fubk16x9_line;
  t.palSize = FUBK16X9_PAL_SIZE;
  t.runCount = FUBK16X9_RUN_COUNT;
  t.uniqueLines = FUBK16X9_UNIQUE_LINES;
  romLoad(&t);
  fubk16x9Reflow();
}

static void fubk16x9RenderRow(uint8_t *base, int y) {
  romRenderRow(base, y);
  textDrawRow(base, &idLine, y, textInkId());
  textDrawRow(base, &subLine, y, textInkSub());
  movingLineDrawRow(base, y);   // last, so the stripe stays unbroken over text
}

const Pattern PATTERN_FUBK16X9 = {LANG_CARD_FUBK_16X9, fubk16x9Init, fubk16x9RenderRow, fubk16x9Tick,
                                  false, false, fubk16x9Reflow};

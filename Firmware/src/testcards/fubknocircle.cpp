// FUBK 4:3 without the centre circle: the same picture as
// PATTERN_FUBK4X3 with the white ring and the piece of central grid
// line under the text box edited out (see tools/make_fubk_nocircle.py),
// everything else byte for byte identical. Fewer unique lines than the
// card it's derived from (237 vs 464), so 76 kB of flash against 146.
//
// The two striped fields right of the gradient bars are the Unbunt
// fields; see fubk4x3.cpp for why they must not be smoothed out.
#include "../config.h"
#include "fubknocircle_data.h"
#include "../gfx.h"
#include "../language.h"
#include "../movingline.h"
#include "../pattern.h"
#include "../rompattern.h"
#include "../settings.h"
#include "../textoverlay.h"

static_assert(FUBKNOCIRCLE_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(FUBKNOCIRCLE_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(FUBKNOCIRCLE_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

// The black text box, from the data itself: column 231..489, row
// 285..323. Twice as wide as FUBK 4:3's own box, since there the
// central grid line splits it and text has to live in the left half.
static const int ID_BOX_X0 = 230, ID_BOX_X1 = 490;
static const int ID_BOX_Y0 = 285, ID_BOX_Y1 = 324;

static TextOverlay idLine;

// The light half of the init: only the text layout. See pattern.h.
static void fubkNoCircleReflow() {
  textPrepare(&idLine, cfgTextIdShown(), ID_BOX_X0, ID_BOX_X1, ID_BOX_Y0, ID_BOX_Y1);
}

// The moving line sweeps the whole black box.
static void fubkNoCircleTick() {
  movingLineBox(ID_BOX_X0, ID_BOX_X1, ID_BOX_Y0, ID_BOX_Y1);
}

static void fubkNoCircleInit() {
  RomTables t;
  t.pal = fubknocircle_pal;
  t.idx = fubknocircle_idx;
  t.len = fubknocircle_len;
  t.start = fubknocircle_start;
  t.line = fubknocircle_line;
  t.palSize = FUBKNOCIRCLE_PAL_SIZE;
  t.runCount = FUBKNOCIRCLE_RUN_COUNT;
  t.uniqueLines = FUBKNOCIRCLE_UNIQUE_LINES;
  romLoad(&t);
  fubkNoCircleReflow();
}

static void fubkNoCircleRenderRow(uint8_t *base, int y) {
  romRenderRow(base, y);
  textDrawRow(base, &idLine, y, textInkId());
  movingLineDrawRow(base, y);   // last, so the stripe stays unbroken over text
}

const Pattern PATTERN_FUBKNOCIRCLE = {LANG_CARD_SIMPLE_FUBK, fubkNoCircleInit,
                                      fubkNoCircleRenderRow, fubkNoCircleTick, false, false,
                                      fubkNoCircleReflow};

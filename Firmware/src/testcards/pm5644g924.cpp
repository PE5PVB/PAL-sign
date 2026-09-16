// The PM5644 test card of the newest generation (G924): near enough
// the same as G00 to look at, but the EPROMs draw the boxes in
// different places and the picture is finer. Four ROM patterns exist
// (pat0 full, pat1/pat2 differ only in two rows, pat3 = pat0 without
// the chroma test bars); only pat0 and pat3 are baked in, chosen with
// CFG_G924_CHROMA_BARS. The chroma test bars are the PAL switch test
// (R-Y/B-Y sign flips every line; noise-like in the raw data, an even
// colour on a good decoder). No insert boxes in the ROM data itself,
// so the clock is drawn on top here, not baked in.
//
// Like G913 this generation samples at just over 20 MHz rather than
// 13.5 MHz, so samples do not pass through one for one;
// tools/convert_rom_pattern.py averages down to 707 samples and 64
// luma levels.
#include "../config.h"
#include "../gfx.h"
#include "../language.h"
#include "../pattern.h"
#include "pm5644g924_data.h"
#include "../rompattern.h"
#include "../inserts.h"
#include "../movingline.h"
#include "../settings.h"
#include "../textoverlay.h"

static_assert(PM5644G924_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(PM5644G924_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(PM5644G924_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

// Measured in the ROM data: the largest contiguous black rectangle per
// area.
static const int ID_BOX_X0 = 303, ID_BOX_X1 = 416;
static const int ID_BOX_Y0 = 59, ID_BOX_Y1 = 101;
static const int SUB_BOX_X0 = 274, SUB_BOX_X1 = 445;
static const int SUB_BOX_Y0 = 437, SUB_BOX_Y1 = 479;

static TextOverlay idLine, subLine, insLeft, insRight;

// Unlike G00 this card draws no black box for the insert fields, only a
// thin marking line (rows 290-291, x 203-314 date, 405-516 time); the
// box drawn here is our own, sized like G00's (42 rows) and centred on
// that marking.
static const int INS_L_X0 = 203, INS_L_X1 = 314;
static const int INS_R_X0 = 405, INS_R_X1 = 516;
static const int INS_Y0 = 270, INS_Y1 = 312;

// pat0 date+time, pat1 time only, pat2 neither, pat3 = pat0 without
// chroma bars (the only bars-off variant, so bars off implies date and
// time on).
static int variantFor() {
  if (!cfgG924ChromaBars()) return 3;
  switch (insertGetMode()) {
    case INSERT_NONE: return 2;
    case INSERT_TIME: return 1;
    default: return 0;
  }
}

// The light half of the init: only the text layout. See pattern.h.
static void pm5644g924Reflow() {
  textPrepare(&idLine, cfgTextIdShown(), ID_BOX_X0, ID_BOX_X1, ID_BOX_Y0, ID_BOX_Y1);
  textPrepare(&subLine, cfgTextSubShown(), SUB_BOX_X0, SUB_BOX_X1, SUB_BOX_Y0, SUB_BOX_Y1);
}

static void pm5644g924Tick() {
  insertPrepare(&insLeft, &insRight, INS_L_X0, INS_L_X1, INS_R_X0, INS_R_X1, INS_Y0, INS_Y1);
  movingLineBox(SUB_BOX_X0, SUB_BOX_X1, SUB_BOX_Y0, SUB_BOX_Y1);
}

static void pm5644g924Init() {
  RomTables t;
  t.pal = pm5644g924_pal;
  t.idx = pm5644g924_idx;
  t.len = pm5644g924_len;
  t.start = pm5644g924_start;
  t.line = pm5644g924_line + (unsigned)variantFor() * ROM_ROWS;
  t.palSize = PM5644G924_PAL_SIZE;
  t.runCount = PM5644G924_RUN_COUNT;
  t.uniqueLines = PM5644G924_UNIQUE_LINES;
  romLoad(&t);

  pm5644g924Reflow();

  insLeft.count = 0;
  insRight.count = 0;
  insertForceRefresh();
  pm5644g924Tick();
}

static void pm5644g924RenderRow(uint8_t *base, int y) {
  romRenderRow(base, y);
  if (insLeft.count) textFillBox(base, INS_L_X0, INS_L_X1, INS_Y0, INS_Y1, y, GFX_BLACK);
  if (insRight.count) textFillBox(base, INS_R_X0, INS_R_X1, INS_Y0, INS_Y1, y, GFX_BLACK);
  textDrawRow(base, &idLine, y, textInkId());
  textDrawRow(base, &subLine, y, textInkSub());
  textDrawRow(base, &insLeft, y, textInkInsert());
  textDrawRow(base, &insRight, y, textInkInsert());
  movingLineDrawRow(base, y);   // last, so the stripe crosses the text unbroken
}

const Pattern PATTERN_PM5644G924 = {LANG_CARD_PM5644_16X9, pm5644g924Init, pm5644g924RenderRow,
                                    pm5644g924Tick, false, false, pm5644g924Reflow};

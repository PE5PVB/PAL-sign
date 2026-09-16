// BBC Test Card G, derived from the PM5644 G00 EPROM tables by
// tools/make_testcardg.py: colour bars/block wave/bottom segment lifted
// 25% (set-up), multiburst redrawn at 71.4% with the PAL-I frequencies
// (1.5/2.5/3.5/4.0/4.5/5.25 MHz); everything else is the EPROM picture
// unchanged. tcgap2 is a sleeper: stored, dumped, never read (one
// variant only).
#include "../config.h"
#include "../gfx.h"
#include "../language.h"
#include "../movingline.h"
#include "../pattern.h"
#include "../rompattern.h"
#include "../settings.h"
#include "testcardg_data.h"
#include "../textoverlay.h"

static_assert(TESTCARDG_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(TESTCARDG_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(TESTCARDG_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

// The text boxes of the PM5644 G00; see pm5644g00.cpp.
static const int ID_BOX_X0 = 285, ID_BOX_X1 = 434;
static const int ID_BOX_Y0 = 55, ID_BOX_Y1 = 97;
static const int SUB_BOX_X0 = 266, SUB_BOX_X1 = 473;
static const int SUB_BOX_Y0 = 433, SUB_BOX_Y1 = 475;

static TextOverlay idLine, subLine;

// The light half of the init: only the text layout. See pattern.h.
static void testcardgReflow() {
  textPrepare(&idLine, cfgTextIdShown(), ID_BOX_X0, ID_BOX_X1, ID_BOX_Y0, ID_BOX_Y1);
  textPrepare(&subLine, cfgTextSubShown(), SUB_BOX_X0, SUB_BOX_X1, SUB_BOX_Y0, SUB_BOX_Y1);
}

static void testcardgTick() {
  movingLineBox(SUB_BOX_X0, SUB_BOX_X1, SUB_BOX_Y0, SUB_BOX_Y1);
}

static void testcardgInit() {
  RomTables t;
  t.pal = testcardg_pal;
  t.idx = testcardg_idx;
  t.len = testcardg_len;
  t.start = testcardg_start;
  t.line = testcardg_line;
  t.palSize = TESTCARDG_PAL_SIZE;
  t.runCount = TESTCARDG_RUN_COUNT;
  t.uniqueLines = TESTCARDG_UNIQUE_LINES;
  romLoad(&t);
  testcardgReflow();
}

static void testcardgRenderRow(uint8_t *base, int y) {
  romRenderRow(base, y);
  textDrawRow(base, &idLine, y, textInkId());
  textDrawRow(base, &subLine, y, textInkSub());
  // Last, so the stripe stays unbroken where it crosses the text.
  movingLineDrawRow(base, y);
}

const Pattern PATTERN_TESTCARDG = {LANG_CARD_BBC_G, testcardgInit, testcardgRenderRow, testcardgTick,
                                   false, false, testcardgReflow};

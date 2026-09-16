// BBC Test Card W, the 16:9 version of Test Card F, built by
// tools/make_testcardw.py from a 1280x720 HD source and the same photo
// scan as F, fitted separately since the anamorphic ratio differs.
#include <string.h>

#include "../gfx.h"
#include "../language.h"
#include "../movingline.h"
#include "../pattern.h"
#include "../portrait.h"
#include "../rompattern.h"
#include "../settings.h"
#include "testcardw_data.h"
#include "testcardw_photo.h"
#include "../textoverlay.h"

static_assert(TESTCARDW_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(TESTCARDW_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(TESTCARDW_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

// Text box under the letter W (key N, cfgTextId), drawn per row like on
// Test Card F. Measured the same way (largest grey rectangle under the
// grid); off-centre by 1px because W's grid pitch does not fall
// symmetrically about the screen centre.
static const int TXT_X0 = 160, TXT_X1 = 559;    // [x0, x1)
static const int TXT_Y0 = 484, TXT_Y1 = 542;    // [y0, y1)

static TextOverlay textLine;

// The light half of the init: only the text layout. See pattern.h.
static void testcardwReflow() {
  textPrepare(&textLine, cfgTextIdShown(), TXT_X0, TXT_X1, TXT_Y0, TXT_Y1);
}

static void testcardwTick() { movingLineBox(TXT_X0, TXT_X1, TXT_Y0, TXT_Y1); }

static void testcardwInit() {
  RomTables t;
  t.pal = testcardw_pal;
  t.idx = testcardw_idx;
  t.len = testcardw_len;
  t.start = testcardw_start;
  t.line = testcardw_line;
  t.palSize = TESTCARDW_PAL_SIZE;
  t.runCount = TESTCARDW_RUN_COUNT;
  t.uniqueLines = TESTCARDW_UNIQUE_LINES;
  romLoad(&t);
  testcardwReflow();
}

static void testcardwRenderRow(uint8_t *base, int y) {
  romRenderRow(base, y);
  int r = y - TESTCARDW_PHOTO_Y0;
  if (r >= 0 && r < TESTCARDW_PHOTO_ROWS) {
    // See testcardf.cpp's own version of this swap for why the same
    // tables place either source.
    const bool custom = cfgCardCustomPhoto(-1) && portraitValid(PORTRAIT_TESTCARDW);
    const uint8_t *src = custom ? portraitData(PORTRAIT_TESTCARDW) : testcardw_photo;
    memcpy(base + (unsigned)testcardw_photo_x0[r] * 2, src + testcardw_photo_off[r],
           (unsigned)testcardw_photo_len[r] * 2);
  }
  textDrawRow(base, &textLine, y, textInkId());
  movingLineDrawRow(base, y);   // last, so the stripe crosses the text unbroken
}

const Pattern PATTERN_TESTCARDW = {LANG_CARD_BBC_W, testcardwInit, testcardwRenderRow,
                                   testcardwTick, false, false, testcardwReflow};

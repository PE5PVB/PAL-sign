// BBC Test Card F, built by tools/make_testcardf.py from Tcfm.jpg (BATC
// wiki) and a separate 304x304 photo scan. The centre photograph and
// the "F" are pixel data, the rest of the card is generator-drawn and
// RLE-baked.
#include <string.h>

#include "../gfx.h"
#include "../language.h"
#include "../movingline.h"
#include "../pattern.h"
#include "../portrait.h"
#include "../rompattern.h"
#include "../settings.h"
#include "testcardf_data.h"
#include "testcardf_photo.h"
#include "../textoverlay.h"

static_assert(TESTCARDF_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(TESTCARDF_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(TESTCARDF_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

// Text box under the letter F (key N, cfgTextId), drawn per row rather
// than baked in so it can change without regenerating the card.
// Measured as the largest grey rectangle under the grid line at y=479.
static const int TXT_X0 = 160, TXT_X1 = 560;    // [x0, x1)
static const int TXT_Y0 = 482, TXT_Y1 = 540;    // [y0, y1)

static TextOverlay textLine;

// The light half of the init: only the text layout. See pattern.h.
static void testcardfReflow() {
  textPrepare(&textLine, cfgTextIdShown(), TXT_X0, TXT_X1, TXT_Y0, TXT_Y1);
}

static void testcardfTick() { movingLineBox(TXT_X0, TXT_X1, TXT_Y0, TXT_Y1); }

static void testcardfInit() {
  RomTables t;
  t.pal = testcardf_pal;
  t.idx = testcardf_idx;
  t.len = testcardf_len;
  t.start = testcardf_start;
  t.line = testcardf_line;
  t.palSize = TESTCARDF_PAL_SIZE;
  t.runCount = TESTCARDF_RUN_COUNT;
  t.uniqueLines = TESTCARDF_UNIQUE_LINES;
  romLoad(&t);
  testcardfReflow();
}

static void testcardfRenderRow(uint8_t *base, int y) {
  romRenderRow(base, y);
  int r = y - TESTCARDF_PHOTO_Y0;
  if (r >= 0 && r < TESTCARDF_PHOTO_ROWS) {
    // The custom upload is packed in exactly the same per-row shape as
    // the built-in photo (see portrait.h), so the same x0/off/len
    // tables place either one; falls back to the built-in photo when no
    // replacement was ever uploaded, even with the switch on.
    const bool custom = cfgCardCustomPhoto(-1) && portraitValid(PORTRAIT_TESTCARDF);
    const uint8_t *src = custom ? portraitData(PORTRAIT_TESTCARDF) : testcardf_photo;
    memcpy(base + (unsigned)testcardf_photo_x0[r] * 2, src + testcardf_photo_off[r],
           (unsigned)testcardf_photo_len[r] * 2);
  }
  textDrawRow(base, &textLine, y, textInkId());
  movingLineDrawRow(base, y);   // last, so the stripe crosses the text unbroken
}

const Pattern PATTERN_TESTCARDF = {LANG_CARD_BBC_F, testcardfInit, testcardfRenderRow, testcardfTick,
                                   false, false, testcardfReflow};

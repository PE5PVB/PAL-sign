// The PM5644 test card of the older generation (G00), straight out of
// the original Philips EPROMs: EPROM dumps converted to BT.601 levels,
// sampled at exactly 13.5 MHz so they pass through BT.656 one for one.
// The two narrow chroma test bars left/right are the PAL switch test
// from the specification (deliberately noise-like per line).
#include <pico.h>
#include <string.h>

#include <Arduino.h>

#include "../clock.h"
#include "../config.h"
#include "../gfx.h"
#include "../inserts.h"
#include "../language.h"
#include "../movingline.h"
#include "../pattern.h"
#include "pm5644g00_data.h"
#include "../rompattern.h"
#include "../settings.h"
#include "../textoverlay.h"

static_assert(PM5644G00_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(PM5644G00_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(PM5644G00_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

// Text boxes, measured in the ROM data itself (largest contiguous black
// rectangle per area), not estimated.
static const int ID_BOX_X0 = 285, ID_BOX_X1 = 434;
static const int ID_BOX_Y0 = 55, ID_BOX_Y1 = 97;
static const int SUB_BOX_X0 = 266, SUB_BOX_X1 = 473;
static const int SUB_BOX_Y0 = 433, SUB_BOX_Y1 = 475;
static const int INS_L_X0 = 151, INS_L_X1 = 299;
static const int INS_R_X0 = 420, INS_R_X1 = 568;
static const int INS_Y0 = 267, INS_Y1 = 307;

static TextOverlay idLine, subLine, insLeft, insRight;

// All three row indices in RAM (3456 bytes), so switching variant
// touches no flash.
static uint16_t lineInRam[3][ROM_ROWS];
static bool lineRamReady = false;

// Which EPROM variant belongs to which mode; the black boxes are part
// of the picture data, so this switches with it.
static int variantFor(int m) {
  switch (m) {
    case INSERT_NONE: return 2;   // pat2, the grid carries on
    case INSERT_TIME: return 1;   // pat1, the right hand box only
    default: return 0;            // pat0, both boxes
  }
}

// The light half of the init: only the text layout. See pattern.h.
static void pm5644g00Reflow() {
  textPrepare(&idLine, cfgTextIdShown(), ID_BOX_X0, ID_BOX_X1, ID_BOX_Y0, ID_BOX_Y1);
  textPrepare(&subLine, cfgTextSubShown(), SUB_BOX_X0, SUB_BOX_X1, SUB_BOX_Y0, SUB_BOX_Y1);
}

static void pm5644g00Tick() {
  insertPrepare(&insLeft, &insRight, INS_L_X0, INS_L_X1, INS_R_X0, INS_R_X1, INS_Y0, INS_Y1);
  movingLineBox(SUB_BOX_X0, SUB_BOX_X1, SUB_BOX_Y0, SUB_BOX_Y1);
}

static void pm5644g00Init() {
  if (!lineRamReady) {
    memcpy(lineInRam, pm5644g00_line, sizeof(lineInRam));
    lineRamReady = true;
  }

  RomTables t;
  t.pal = pm5644g00_pal;
  t.idx = pm5644g00_idx;
  t.len = pm5644g00_len;
  t.start = pm5644g00_start;
  t.line = lineInRam[variantFor(insertGetMode())];
  t.palSize = PM5644G00_PAL_SIZE;
  t.runCount = PM5644G00_RUN_COUNT;
  t.uniqueLines = PM5644G00_UNIQUE_LINES;
  romLoad(&t);

  // Once per boot: catches a stale build whose header promises more
  // rows than flash actually holds.
  static bool checked = false;
  if (!checked) {
    checked = true;
    romCopyFinish();
    int line = 0, sum = 0;
    int wrong = romSelfCheck(&line, &sum);
    if (wrong) {
      Serial.printf(LANG_ROM_NOTE_D_WRONG_IN,
                    wrong, line, sum);
      Serial.println(LANG_THE_DATA_IN_FLASH);
      Serial.println(LANG_TOOLS_CONVERT_ROM_PATTERN);
    }
  }

  pm5644g00Reflow();

  insLeft.count = 0;
  insRight.count = 0;
  insertForceRefresh();
  pm5644g00Tick();
}

static void pm5644g00RenderRow(uint8_t *base, int y) {
  romRenderRow(base, y);
  textDrawRow(base, &idLine, y, textInkId());
  textDrawRow(base, &subLine, y, textInkSub());
  textDrawRow(base, &insLeft, y, textInkInsert());
  textDrawRow(base, &insRight, y, textInkInsert());
  movingLineDrawRow(base, y);   // last, so the stripe crosses the text unbroken
}

const Pattern PATTERN_PM5644G00 = {LANG_CARD_PM5644, pm5644g00Init, pm5644g00RenderRow, pm5644g00Tick,
                                   false, false, pm5644g00Reflow};

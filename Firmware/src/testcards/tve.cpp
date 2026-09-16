// TVE Carta de Ajuste, the Spanish colour test card, designed 1973 by
// Finn Hendil at Philips (same team as the PM5544). No pattern ROM of
// the original survives, so tools/make_tve.py draws it from a measured
// recreation and the card's published spec.
//
// No black boxes behind the text: it's written straight onto the card
// (textDrawRow() writes solid YCbCr including chroma), so white text on
// the blue circle stays white, at the cost of a half-sample colour
// fringe from the 4:2:2 chroma pairing.
#include "../clock.h"
#include "../config.h"
#include "../gfx.h"
#include "../inserts.h"
#include "../language.h"
#include "../movingline.h"
#include "../pattern.h"
#include "../rompattern.h"
#include "../settings.h"
#include "../textoverlay.h"
#include "tve_data.h"

static_assert(TVE_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(TVE_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(TVE_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

// The four text/insert areas; both fit inside the circle with margin to
// spare. make_tve.py prints these same numbers when it runs.
static const int TXT1_X0 = 216, TXT1_X1 = 503;
static const int TXT1_Y0 = 104, TXT1_Y1 = 164;
static const int TXT2_X0 = 286, TXT2_X1 = 433;
static const int TXT2_Y0 = 490, TXT2_Y1 = 524;
static const int INS_L_X0 = 58, INS_L_X1 = 217;
static const int INS_R_X0 = 502, INS_R_X1 = 662;
static const int INS_Y0 = 484, INS_Y1 = 520;

static TextOverlay line1, line2, insLeft, insRight;

// The black bar (Y 16, x 228..490, rows 448..488) and its 2T pulse
// (x 357..361). The moving line takes the pulse's place while it runs;
// the sweep box stops one column short of the bar's own edge (490,
// where the chroma pair already straddles the coloured area beside it)
// to keep everything the luma-only stripe touches neutral.
static const int BAR_X0 = 228, BAR_X1 = 489;
static const int BAR_Y0 = 448, BAR_Y1 = 489;
static const int PULSE_X0 = 356, PULSE_X1 = 363;

// The light half of the init: only the text layout. See pattern.h.
static void tveReflow() {
  textPrepare(&line1, cfgTextIdShown(), TXT1_X0, TXT1_X1, TXT1_Y0, TXT1_Y1);
  textPrepare(&line2, cfgTextSubShown(), TXT2_X0, TXT2_X1, TXT2_Y0, TXT2_Y1);
}

static void tveTick() {
  insertPrepare(&insLeft, &insRight, INS_L_X0, INS_L_X1, INS_R_X0, INS_R_X1, INS_Y0, INS_Y1);
  movingLineBox(BAR_X0, BAR_X1, BAR_Y0, BAR_Y1);
}

static void tveInit() {
  RomTables t;
  t.pal = tve_pal;
  t.idx = tve_idx;
  t.len = tve_len;
  t.start = tve_start;
  t.line = tve_line;
  t.palSize = TVE_PAL_SIZE;
  t.runCount = TVE_RUN_COUNT;
  t.uniqueLines = TVE_UNIQUE_LINES;
  romLoad(&t);

  tveReflow();

  insLeft.count = 0;
  insRight.count = 0;
  insertForceRefresh();
  tveTick();
}

static void tveRenderRow(uint8_t *base, int y) {
  romRenderRow(base, y);
  if (movingLineActive() && y >= BAR_Y0 && y < BAR_Y1) {
    for (int x = PULSE_X0; x < PULSE_X1; x++)
      base[(x >> 1) * 4 + ((x & 1) ? 3 : 1)] = 16;
  }
  textDrawRow(base, &line1, y, textInkId());
  textDrawRow(base, &line2, y, textInkSub());
  // The date/time sit on the grid, half on grey and half on a white
  // grid line, so they get a thicker (2, not the default 1) black
  // outline first to stay readable there.
  textDrawRowOutline(base, &insLeft, y, GFX_BLACK, 2);
  textDrawRowOutline(base, &insRight, y, GFX_BLACK, 2);
  textDrawRow(base, &insLeft, y, textInkInsert());
  textDrawRow(base, &insRight, y, textInkInsert());
  movingLineDrawRow(base, y);   // last, so the stripe stays unbroken
}

const Pattern PATTERN_TVE = {LANG_CARD_TVE, tveInit, tveRenderRow, tveTick, false, false,
                             tveReflow};

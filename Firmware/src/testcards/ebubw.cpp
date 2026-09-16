// The EBU black and white test card, drawn by tools/make_ebubw.py from
// tools/EBU_BW_Test_Card.jpg (699x575).
//
//   row   0.. 47  grating 0,8 MHz
//   row  48..107  grating 1,8 MHz
//   row 108..167  grating 2,8 MHz
//   row 168..227  grating 3,8 MHz
//   row 228..287  grating 4,8 MHz
//   row 288..407  black bar, a white block left (x 0..107) and right
//                 (x 622..719), a white line at x 210..212
//   row 408..575  black on the left, a ten step grey staircase right
//
// The gratings (0.8 / 1.8 / 2.8 / 3.8 / 4.8 MHz) are the published
// definition lines of the Philips PM5540 this card comes from; all
// five at the same amplitude, 127.5 +/- 127.5 (black to peak white).
// The grey staircase has no published spec and was measured from the
// reference instead.
//
// Interlace-safe: all seven band boundaries are on an even row.
//
// Seven unique picture rows (five gratings, a bar row, a staircase
// row), 1823 RLE runs, 8 kB of flash, no extra RAM: everything lives in
// rompattern.cpp's shared working copy. A grating row is the busiest in
// the firmware, 360 runs (one word each); the 64us row budget holds.
#include "ebubw_data.h"
#include "../gfx.h"
#include "../language.h"
#include "../pattern.h"
#include "../rompattern.h"
#include "../settings.h"
#include "../textoverlay.h"

static_assert(EBUBW_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(EBUBW_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(EBUBW_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

// The two text boxes in the black bar, right of the white line at
// x 210..212 (writing over it would spoil the measuring element),
// centred on the picture centre (x 360).
static const int TEXT_X0 = 216, TEXT_X1 = 504;
static const int TEXT1_Y0 = 288, TEXT1_Y1 = 348;
static const int TEXT2_Y0 = 348, TEXT2_Y1 = 408;

static TextOverlay idLine, subLine;

// The light half of the init: only the text layout. See pattern.h.
static void ebubwReflow() {
  textPrepare(&idLine, cfgTextIdShown(), TEXT_X0, TEXT_X1, TEXT1_Y0, TEXT1_Y1);
  textPrepare(&subLine, cfgTextSubShown(), TEXT_X0, TEXT_X1, TEXT2_Y0, TEXT2_Y1);
}

static void ebubwInit() {
  RomTables t;
  t.pal = ebubw_pal;
  t.idx = ebubw_idx;
  t.len = ebubw_len;
  t.start = ebubw_start;
  t.line = ebubw_line;  // only one variant
  t.palSize = EBUBW_PAL_SIZE;
  t.runCount = EBUBW_RUN_COUNT;
  t.uniqueLines = EBUBW_UNIQUE_LINES;
  romLoad(&t);
  ebubwReflow();
}

static void ebubwRenderRow(uint8_t *base, int y) {
  romRenderRow(base, y);
  textDrawRow(base, &idLine, y, textInkId());
  textDrawRow(base, &subLine, y, textInkSub());
}

// mono = true: the encoder switches chroma and burst off.
const Pattern PATTERN_EBUBW = {LANG_CARD_EBU_BW, ebubwInit, ebubwRenderRow, nullptr, true, false,
                               ebubwReflow};

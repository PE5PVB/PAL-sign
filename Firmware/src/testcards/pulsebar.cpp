// Pulse and bar: a test signal. A 2T pulse (overshoot/ringing) and a
// white bar (low frequency behaviour), for measuring a chain's linear
// distortion. Monochrome, chroma held neutral.
#include "../language.h"
#include "../pattern.h"
#include "pulsebar_data.h"
#include "../rompattern.h"

static_assert(PULSEBAR_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(PULSEBAR_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(PULSEBAR_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

static void pulsebarInit() {
  RomTables t;
  t.pal = pulsebar_pal;
  t.idx = pulsebar_idx;
  t.len = pulsebar_len;
  t.start = pulsebar_start;
  t.line = pulsebar_line;
  t.palSize = PULSEBAR_PAL_SIZE;
  t.runCount = PULSEBAR_RUN_COUNT;
  t.uniqueLines = PULSEBAR_UNIQUE_LINES;
  romLoad(&t);
}

const Pattern PATTERN_PULSEBAR = {LANG_CARD_PULSE_AND_BAR, pulsebarInit, romRenderRow, nullptr, true, false};

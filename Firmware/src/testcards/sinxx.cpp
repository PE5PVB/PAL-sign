// sin(x)/x: a test signal. A single pulse holds every frequency up to
// the cut-off at the same amplitude, showing a chain's amplitude
// response at a glance. Monochrome.
#include "../language.h"
#include "../pattern.h"
#include "sinxx_data.h"
#include "../rompattern.h"

static_assert(SINXX_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(SINXX_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(SINXX_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

static void sinxxInit() {
  RomTables t;
  t.pal = sinxx_pal;
  t.idx = sinxx_idx;
  t.len = sinxx_len;
  t.start = sinxx_start;
  t.line = sinxx_line;
  t.palSize = SINXX_PAL_SIZE;
  t.runCount = SINXX_RUN_COUNT;
  t.uniqueLines = SINXX_UNIQUE_LINES;
  romLoad(&t);
}

const Pattern PATTERN_SINXX = {LANG_CARD_SINXX, sinxxInit, romRenderRow, nullptr, true, false};

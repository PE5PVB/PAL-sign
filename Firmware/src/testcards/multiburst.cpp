// Multiburst: a black and white reference block, then six bursts of
// sine at rising frequency (0.5/1/2/3/4/4.8 MHz) at the same amplitude,
// showing the chain's frequency response at a glance (PAL B/G passes
// 5 MHz, so the last burst should just about survive). Every row is
// identical, computed rather than scanned; see
// build_frame_multiburst() in tools/convert_rom_pattern.py.
#include "multiburst_data.h"
#include "../language.h"
#include "../pattern.h"
#include "../rompattern.h"

static_assert(MULTIBURST_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(MULTIBURST_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(MULTIBURST_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

static void multiburstInit() {
  RomTables t;
  t.pal = multiburst_pal;
  t.idx = multiburst_idx;
  t.len = multiburst_len;
  t.start = multiburst_start;
  t.line = multiburst_line;
  t.palSize = MULTIBURST_PAL_SIZE;
  t.runCount = MULTIBURST_RUN_COUNT;
  t.uniqueLines = MULTIBURST_UNIQUE_LINES;
  romLoad(&t);
}

const Pattern PATTERN_MULTIBURST = {LANG_CARD_MULTIBURST, multiburstInit, romRenderRow, nullptr, true,
                                    false};

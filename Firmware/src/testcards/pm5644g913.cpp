// PM5644 G913: the monoscope with the Indian head, resolution wedges
// and numbered scales, entirely monochrome (Cb/Cr = 128 throughout).
// Samples at just over 20 MHz rather than 13.5 MHz, so unlike G00 they
// do not pass through one for one; tools/convert_rom_pattern.py
// averages down to 707 samples and 64 luma levels.
#include "../language.h"
#include "../pattern.h"
#include "pm5644g913_data.h"
#include "../rompattern.h"

static_assert(PM5644G913_RUN_COUNT <= ROM_MAX_RUNS, "raise ROM_MAX_RUNS in rompattern.h");
static_assert(PM5644G913_PAL_SIZE <= ROM_MAX_PAL, "raise ROM_MAX_PAL in rompattern.h");
static_assert(PM5644G913_UNIQUE_LINES <= ROM_MAX_UNIQUE_LINES,
              "raise ROM_MAX_UNIQUE_LINES in rompattern.h");

static void pm5644g913Init() {
  RomTables t;
  t.pal = pm5644g913_pal;
  t.idx = pm5644g913_idx;
  t.len = pm5644g913_len;
  t.start = pm5644g913_start;
  t.line = pm5644g913_line;  // only one variant
  t.palSize = PM5644G913_PAL_SIZE;
  t.runCount = PM5644G913_RUN_COUNT;
  t.uniqueLines = PM5644G913_UNIQUE_LINES;
  romLoad(&t);
}

// No text overlay, so the decoder can write straight into it.
const Pattern PATTERN_PM5644G913 = {LANG_CARD_INDIAN_HEAD_BW, pm5644g913Init, romRenderRow,
                                    nullptr, true, false};

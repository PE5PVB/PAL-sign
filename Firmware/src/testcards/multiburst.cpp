// Multiburst: a full-field PAL multiburst. A black (0 V) and a white
// (0.7 V) reference bar, then six bursts on a 50% pedestal at
// 0.5/1.5/2.5/3.5/4.5/5.5 MHz, full amplitude so the peaks reach white
// and the troughs reach black; the white bar is the amplitude
// reference. Every row is identical. The inserted ITS line 18 keeps
// its own J.63 signal; see build_frame_multiburst() in
// tools/convert_rom_pattern.py.
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

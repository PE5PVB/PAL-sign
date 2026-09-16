// Playing back a test card that comes from an EPROM dump. Every ROM
// test card delivers the same five tables (see
// tools/convert_rom_pattern.py) and this file unpacks them; whatever
// goes on top, text in black boxes or a running clock, is the
// pattern's own business.
//
// Adding a ROM test card:
//   1. put the set in SETS in tools/convert_rom_pattern.py and run it
//   2. write a .cpp modelled on pm5644g00.cpp
//   3. put the card in the table in main.cpp
#ifndef ROMPATTERN_H
#define ROMPATTERN_H

#include <stdint.h>

// Working memory for the ACTIVE test card only, so these are the
// maximum over all baked-in cards, not their sum. A new, bigger card
// trips the static_assert in its own .cpp.
static const int ROM_MAX_RUNS = 87000;
static const int ROM_MAX_PAL = 3900;
static const int ROM_MAX_UNIQUE_LINES = 600;
static const int ROM_ROWS = 576;

// Pointers to a test card's tables as they sit in flash. Everything
// except line[] is shared by the variants.
struct RomTables {
  const uint32_t *pal;    // palette: one pixel pair (Cb,Y,Cr,Y) per entry
  const uint16_t *idx;    // palette index per run
  const uint8_t *len;     // number of repeats per run
  const uint32_t *start;  // first run per unique row (plus a closing value)
  const uint16_t *line;   // row -> unique row, of the chosen variant
  int palSize;
  int runCount;
  int uniqueLines;
};

// Puts a test card up: points the renderer at the tables in flash at
// once, then copies them to RAM as a background job via romCopyStep()
// so nobody stalls waiting on the copy. Decoding from flash costs more
// per row but stays under the 64 us budget.
void romLoad(const RomTables *t);

bool romCopyPending(void);

// Copies at most maxBytes and returns whether anything is left. Meant
// to be called from the producer when the ring buffer has rows to
// spare; see video.cpp.
bool romCopyStep(uint32_t maxBytes);

// Finishes the copy now, however long it takes: for callers that need
// the tables in RAM (romSelfCheck()) or a card too slow to decode from
// flash within a row.
void romCopyFinish(void);

bool romCopyForced(void);   // did the last card change fall back on that? report only

// Like romCopyForced(), but only when the producer ran out of stock.
bool romCopyPanicked(void);
void romCopyPanic(void);

// Replaces the row index only (1152 bytes), without copying the big
// tables again: switching variant, e.g. a card with/without insert
// boxes.
void romSetLines(const uint16_t *line);

// Checks what is really in RAM (finish the copy first): every row adds
// up to 360 words, no row index points outside the table. Returns the
// fault count, first case in firstLine/firstSum.
int romSelfCheck(int *firstLine, int *firstSum);

// Fills picture row y (0..575) with 720 pixels in 4:2:2 from the loaded
// card. Text goes over it afterwards.
void romRenderRow(uint8_t *base, int y);

// The same decode loop over tables handed in: romRenderRow() runs it
// over the RAM copies, this over the flash originals while a card
// change is still copying in.
void romRenderFrom(const uint32_t *pal, const uint16_t *idx, const uint8_t *len,
                   const uint32_t *start, const uint16_t *line, int lines, uint8_t *base, int y);
#endif  // ROMPATTERN_H

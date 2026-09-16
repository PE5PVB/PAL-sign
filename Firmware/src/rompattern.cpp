// The renderer works from RAM copies (a row reads scattered runs from
// three tables too far apart for the XIP cache). Only while a card
// change is being copied in does it decode straight from flash, dearer
// per row but still within budget, to keep the picture up during the
// copy.
#include "rompattern.h"

#include <pico.h>
#include <string.h>

// The working copies in RAM, about 156 kB together.
static uint32_t romPal[ROM_MAX_PAL];
static uint16_t romIdx[ROM_MAX_RUNS];
static uint8_t romLen[ROM_MAX_RUNS];
static uint32_t romStart[ROM_MAX_UNIQUE_LINES + 1];
static uint16_t romLine[ROM_ROWS];

static int romLines = 0;   // bounds romLine, so a bad index is a wrong picture, not corruption

// A card with one unique line (Multiburst: 297 runs of sine, decoded
// 15625 times a second) rendered per run costs ~60 us a row, on the
// edge of the 64 us budget and tipped over it by nothing more than a
// build reshuffling the flash cache. Decoded once into a cache, a row
// is a 1440 byte RAM copy instead. The cache lives in the tail of
// romPal below: a new 1440 byte buffer no longer links (RAM is full),
// and a one-line card leaves nearly all of the 3900 palette entries
// unused. Guarded on palSize where it is used.
static const int ROM_LINE_CACHE_WORDS = 360;   // 720 px x 2 bytes / 4
static bool romLineCacheValid = false;

// The descriptor of the card last loaded, so the flash originals can
// still be reached while a card change is being copied in.
static RomTables romSrc = {};

struct CopyStage {
  uint8_t *dst;
  const uint8_t *src;
  uint32_t bytes;
};
static CopyStage romStage[5];
static int romStageNo = -1;   // -1: nothing to do
static uint32_t romStageDone = 0;

// True only while a copy is under way, so the renderer reads flash
// instead of the RAM tables. Both run on core0 in the producer loop, so
// the renderer never catches the tables halfway.
static bool romFromFlash = false;

bool romCopyPending(void) { return romStageNo >= 0; }

// Two flags: romCopyFinish() runs from four places, and only the panic
// branch in loop() means the producer could not keep up; the other
// three (card dump, a known-too-slow card, the self check) must not
// brand a card.
static bool romCopyWasForced = false;
static bool romCopyPanicked_ = false;

bool romCopyForced(void) { return romCopyWasForced; }
bool romCopyPanicked(void) { return romCopyPanicked_; }

void romLoad(const RomTables *t) {
  romSrc = *t;
  romStage[0] = {(uint8_t *)romPal, (const uint8_t *)t->pal,
                 (uint32_t)(t->palSize * sizeof(uint32_t))};
  romStage[1] = {(uint8_t *)romIdx, (const uint8_t *)t->idx,
                 (uint32_t)(t->runCount * sizeof(uint16_t))};
  romStage[2] = {(uint8_t *)romLen, (const uint8_t *)t->len, (uint32_t)t->runCount};
  romStage[3] = {(uint8_t *)romStart, (const uint8_t *)t->start,
                 (uint32_t)((t->uniqueLines + 1) * sizeof(uint32_t))};
  romStage[4] = {(uint8_t *)romLine, (const uint8_t *)t->line,
                 (uint32_t)(ROM_ROWS * sizeof(uint16_t))};
  romStageNo = 0;
  romStageDone = 0;
  romCopyWasForced = false;
  romCopyPanicked_ = false;
  romLineCacheValid = false;
  romFromFlash = true;  // so the card is up at once, while the copy runs
}

bool romCopyStep(uint32_t maxBytes) {
  while (romStageNo >= 0 && maxBytes) {
    CopyStage &s = romStage[romStageNo];
    uint32_t left = s.bytes - romStageDone;
    if (!left) {
      romStageDone = 0;
      if (++romStageNo >= 5) romStageNo = -1;
      continue;
    }
    uint32_t n = (left < maxBytes) ? left : maxBytes;
    memcpy(s.dst + romStageDone, s.src + romStageDone, n);
    romStageDone += n;
    maxBytes -= n;
  }

  // Only now is RAM consistent, so only now may the renderer switch;
  // romLines changes at the same moment.
  if (romStageNo < 0 && romFromFlash) {
    romLines = romSrc.uniqueLines;
    romFromFlash = false;
  }
  return romStageNo >= 0;
}

void romCopyPanic(void) {
  if (romStageNo >= 0) romCopyPanicked_ = true;
  romCopyFinish();
}

void romCopyFinish(void) {
  if (romStageNo >= 0) romCopyWasForced = true;
  while (romCopyStep(0x10000u)) {
  }
}

int romSelfCheck(int *firstLine, int *firstSum) {
  int wrong = 0;
  *firstLine = -1;
  *firstSum = 0;
  for (int li = 0; li < romLines; li++) {
    uint32_t a = romStart[li], b = romStart[li + 1];
    if (b < a || b > (uint32_t)ROM_MAX_RUNS) {
      if (!wrong++) { *firstLine = li; *firstSum = -1; }
      continue;
    }
    int sum = 0;
    for (uint32_t i = a; i < b; i++) sum += romLen[i];
    if (sum != 360) {
      if (!wrong++) { *firstLine = li; *firstSum = sum; }
    }
  }
  for (int y = 0; y < ROM_ROWS; y++) {
    if (romLine[y] >= romLines) {
      if (!wrong++) { *firstLine = -2 - y; *firstSum = romLine[y]; }
    }
  }
  return wrong;
}

void romSetLines(const uint16_t *line) {
  romSrc.line = line;
  romLineCacheValid = false;

  if (romStageNo >= 0) {
    // A copy is still under way: hand the new variant to it rather than
    // writing romLine directly, or the copy overwrites it again.
    romStage[4].src = (const uint8_t *)line;
    if (romStageNo == 4) romStageDone = 0;   // else half of one variant, half of the other
    return;
  }
  memcpy(romLine, line, ROM_ROWS * sizeof(uint16_t));
}

// Tables handed in rather than taken from the globals, so the same code
// runs over both the RAM copies and the flash originals.
void __not_in_flash_func(romRenderFrom)(const uint32_t *pal, const uint16_t *idx,
                                        const uint8_t *len, const uint32_t *start,
                                        const uint16_t *line, int lines, uint8_t *base, int y) {
  int li = line[y];
  if (li >= lines) li = 0;  // safety net, see romLines
  uint32_t i = start[li];
  uint32_t e = start[li + 1];
  uint32_t *out = (uint32_t *)base;
  for (; i < e; i++) {
    uint32_t v = pal[idx[i]];
    int n = len[i];
    // A zero from a corrupted table would wrap the do/while through
    // ~4 billion stores; one predictable branch per run buys a crash
    // guard.
    if (n <= 0) continue;
    do {
      *out++ = v;
    } while (--n);
  }
}

void __not_in_flash_func(romRenderRow)(uint8_t *base, int y) {
  if (romFromFlash) {
    romRenderFrom(romSrc.pal, romSrc.idx, romSrc.len, romSrc.start, romSrc.line,
                  romSrc.uniqueLines, base, y);
    return;
  }
  if (romLines == 1 && romSrc.palSize <= ROM_MAX_PAL - ROM_LINE_CACHE_WORDS) {
    // Built lazily on the first row after the copy: the RAM tables are
    // complete by then, and every y maps to that one line.
    uint8_t *cache = (uint8_t *)&romPal[ROM_MAX_PAL - ROM_LINE_CACHE_WORDS];
    if (!romLineCacheValid) {
      romRenderFrom(romPal, romIdx, romLen, romStart, romLine, romLines, cache, y);
      romLineCacheValid = true;
    }
    memcpy(base, cache, ROM_LINE_CACHE_WORDS * sizeof(uint32_t));
    return;
  }
  romRenderFrom(romPal, romIdx, romLen, romStart, romLine, romLines, base, y);
}

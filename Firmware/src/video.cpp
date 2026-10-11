#include "video.h"

#include "board.h"

#include <Arduino.h>

#include "bt656.pio.h"
#include "config.h"
#include "videoloop.h"
#include "aspect.h"
#include "gfx.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pattern.h"
#include "display.h"
#include "rompattern.h"
#include "its.h"
#include "teletext.h"
#include "hardware/regs/addressmap.h"

static const uint8_t DATA_PIN_BASE = PIN_DATA_BASE;
static const uint8_t CLKOUT_PIN = PIN_CLKOUT;

// SFL timing reset. The encoder holds its SD counters in reset while
// this pin is high and resumes on release at field 1, subcarrier phase
// zero (page 46, 0x84 bits[2:1] = 10). The release must fall on the
// 625 -> 1 line boundary, known only to the streamer on core1.
//
// Phase 1: waiting to drive the pin high. Phase 2: waiting to release it.
static volatile int sflPhase = 0;

static const uint32_t SAMPLES_PER_LINE = VIDEO_SAMPLES_PER_LINE;
static const uint32_t ACTIVE_SAMPLES = 1440;  // 720 px x 2 bytes (4:2:2)
static const uint32_t BLANK_FILL_SAMPLES = 280;
static const uint32_t LINES_PER_FRAME = VIDEO_LINES_PER_FRAME;
static const uint32_t WORDS_PER_LINE = SAMPLES_PER_LINE / 4;

// sysclk / (27 MHz x state-machine cycles per byte): two for the
// straight bus, four for the mirrored one (bt656_out_rev needs the
// extra two for its pull and bit-reverse). Sysclk must divide exactly
// or CLKIN gets jitter; CFG_SYSCLK_HZ and not F_CPU since the Arduino
// IDE cannot offer that multiple itself.
#if CFG_PIXEL_BUS_REVERSED
static const int PIO_CLKDIV = CFG_SYSCLK_HZ / 108000000;
static_assert(CFG_SYSCLK_HZ % 108000000 == 0,
              "sysclk must be a multiple of 108 MHz for exactly 27 MHz CLKIN on a mirrored bus");
#else
static const int PIO_CLKDIV = CFG_SYSCLK_HZ / 54000000;
static_assert(CFG_SYSCLK_HZ % 54000000 == 0,
              "sysclk must be a multiple of 54 MHz (108/162/216) for exactly 27 MHz CLKIN");
#endif

// Ring buffer: core0 renders far ahead, core1 consumes. aligned(4) since
// the filling functions write 32 bit words and 1728 divides by 4.
static const uint32_t RING_SIZE = VIDEO_RING_SIZE;

// Thresholds on the producer's row stock: PANIC falls back to copying a
// ROM card in one go instead of pieces; COPY is where the stock sits
// during a copy; DISPLAY is the floor above which the status display
// may draw.
static const int32_t ROM_COPY_PANIC_ROWS = VIDEO_ROM_COPY_PANIC_ROWS;
static const int32_t ROM_COPY_KEEP_ROWS = VIDEO_ROM_COPY_KEEP_ROWS;
static const int32_t DISPLAY_KEEP_ROWS = VIDEO_DISPLAY_KEEP_ROWS;

static const uint32_t ROM_COPY_CHUNK = VIDEO_ROM_COPY_CHUNK;
static const uint32_t DISPLAY_CHUNK = VIDEO_DISPLAY_CHUNK;

// May the display do any work this row? The display is lowest priority
// on this core: a late row shows on screen, a late thumbnail refresh
// does not.
bool videoDisplayMayWork = false;
bool videoDisplaySample = false;

uint8_t videoRing[RING_SIZE][SAMPLES_PER_LINE] __attribute__((aligned(4)));
volatile uint32_t videoProducedCount = 0;

volatile uint32_t videoConsumedCount = 0;
volatile bool videoRenormRequest = false;   // videoloop.h, the counter wrap guard
static volatile uint32_t starveCount = 0;
static volatile uint32_t txStallCount = 0;
static volatile uint32_t chainRestartCount = 0;
volatile uint32_t videoStallCount = 0;
volatile uint32_t videoResyncCount = 0;   // the producer hooked back on after falling behind
volatile int32_t videoProducerMaxAhead = RING_SIZE - 3;
static volatile uint32_t flashLineCount = 0;
static volatile uint32_t loadStallCount = 0;
// Core1's time spent waiting for the DMA channel, summed, for the
// report's line on how busy each core is.
volatile uint64_t videoCore1WaitUs = 0;

// Two blank rows in RAM, one per DMA channel, their F and V bits in SAV
// and EAV written at load time (H is fixed at 0/1; see eavSavByte()).
// With no fresh row in the ring buffer, the streamer sends one of these
// instead, so CLKIN and the DMA chain never stall. The channel being
// reloaded has finished its last row, so its blank row is free to write
// while the other channel's may still be going out. 2 x 1728 = 3456
// bytes RAM.
static uint8_t blankLine[2][SAMPLES_PER_LINE] __attribute__((aligned(4)));

static PIO pio = pio0;
static uint pioSm = 0;
int videoDmaChanA = -1, videoDmaChanB = -1;
static volatile bool clockReady = false;

// Set the moment the PIO has to wait on an empty TX FIFO: a hard
// measurement of every CLKIN stall.
static const uint32_t TXSTALL_BIT = 1u << (PIO_FDEBUG_TXSTALL_LSB + 0);

// Timing instrumentation.
volatile uint32_t videoWorstBuildUs = 0;
volatile uint32_t videoMinBuildUs = 0xFFFFFFFF;
volatile uint64_t videoSumBuildUs = 0;
volatile uint32_t videoSampleCount = 0;
volatile uint32_t videoOverBudgetCount = 0;

bool videoClockReady() { return clockReady; }

void videoGetStats(VideoStats *s) {
  uint32_t cnt = videoSampleCount;
  s->minUs = videoMinBuildUs;
  s->worstUs = videoWorstBuildUs;
  s->avgUs = cnt ? (uint32_t)(videoSumBuildUs / cnt) : 0;
  s->overBudget = videoOverBudgetCount;
  s->lines = cnt;
  s->starves = starveCount;
  s->txStalls = txStallCount;
  s->chainRestarts = chainRestartCount;
  s->producerStalls = videoStallCount;
  s->flashLines = flashLineCount;
  s->loadStalls = loadStallCount;
}

ALWAYS_INLINE static uint8_t eavSavByte(bool F, bool V, bool H) {
  uint8_t P3 = V ^ H;
  uint8_t P2 = F ^ H;
  uint8_t P1 = F ^ V;
  uint8_t P0 = F ^ V ^ H;
  return 0x80 | (F << 6) | (V << 5) | (H << 4) | (P3 << 3) | (P2 << 2) | (P1 << 1) | P0;
}

// Samples that lie inside analogue line blanking, put back to blanking
// level: BT.601-7 table 3 vs BT.470-6 table 1-1 puts samples 0..9 and
// 712..719 of our 720 there. IN RAM with the rest of the row path.
static inline void __not_in_flash_func(blankEdges)(uint8_t *active) {
  uint32_t *w = (uint32_t *)active;
  for (int i = 0; i < 5; i++) w[i] = 0x10801080u;              // samples 0..9
  for (int i = 356; i < 360; i++) w[i] = 0x10801080u;          // samples 712..719
}

// ---------------------------------------------------------------------
// A photograph's rows, copied into the ring ahead of core0 by core1.
//
// A photograph row is a 1440 byte copy out of XIP, 44 us of the 64 a row
// has, so core0 never gets more than a few rows ahead of the streamer,
// and a transparent band laid on by core1 has no room left to work in.
// Core1 waits on the DMA for most of every row; here it copies the
// picture rows into the ring slots ahead of the row core0 is building,
// and core0 finds its row already there.
//
// The flash is the one bottleneck, one row per 44 us whoever reads it,
// so the two cores never copy against each other: core1 copies row
// after row, core0 copies a row itself only when core1 is not on it,
// and when core1 is, core0 waits and core1 finishes that row first
// thing next turn. On a turn the band's overlay takes, core1 begins no
// new row, so core0 copies the band's rows itself, as before.
//
// Claiming a slot is Dekker's handshake over two flags: core1 writes
// prefillBusy = q, barrier, then reads videoProducedCount and backs off
// when core0 is at q or past it; core0 has written videoProducedCount
// = q before starting the row, barrier, then reads prefillBusy. With a
// full barrier on both sides at least one of them sees the other's
// write. Core1 keeps the claim across turns until the row is done.
//
// A slot counts as filled when its tag holds the row counter it was
// filled for and the generation of the source at the time (which
// picture, which letterbox): a slot left over from before any change
// never passes, and core0 blanks the tag of every slot it fills itself.
static const int32_t PREFILL_LEAD = 1;
static const uint32_t PREFILL_NONE = 0xFFFFFFFFu;
// In pieces, with a look at the DMA in between: 40 words are some 5 us
// with nothing else on the flash, so a reload waits at most that long.
// Four words a go: a word at a time is a bus transaction per word and
// came to 69 us a row, slower than the line; the four-word block below
// compiles to ldmia/stmia.
static const uint32_t PREFILL_CHUNK_WORDS = 40;
static const uint32_t PREFILL_CHUNKS = ACTIVE_SAMPLES / 4 / PREFILL_CHUNK_WORDS;
static_assert(PREFILL_CHUNK_WORDS * PREFILL_CHUNKS * 4 == ACTIVE_SAMPLES, "chunks must cover the row");
struct PrefillBlock {
  uint32_t w[4];
};
static_assert(PREFILL_CHUNK_WORDS % 4 == 0, "a chunk is whole blocks");
// Read the picture through the no-allocate window, so core1's copies do
// not push core0's code out of the XIP cache.
static const bool PREFILL_NOALLOC = false;

static const uint8_t *volatile prefillSrc = nullptr;   // the picture's row 0, or off
static volatile int prefillBar = 0, prefillActive = SCREEN_H;
static volatile uint32_t prefillGen = 0;
static volatile uint32_t prefillTag[RING_SIZE];
static volatile uint8_t prefillTagGen[RING_SIZE];
static volatile uint32_t prefillBusy = PREFILL_NONE;   // the slot core1 is copying into
static uint32_t prefillCursor = 0;                     // core1's: the next slot to take
static bool rowPrefilled = false;
static volatile uint32_t prefillByCore1 = 0, prefillByCore0 = 0, prefillCut = 0;
static uint32_t prefillWaits = 0, prefillWaitUs = 0;   // core0's

// Only a real change moves the generation on: both are called again
// with the same values as a matter of course (every card change sets
// the aspect, a text change runs init() again), and every step of the
// generation throws away the rows copied in so far.
void videoSetPrefill(const uint8_t *photo) {
  if (photo == prefillSrc) return;
  // One slot of lead for core1 (videoloop.h): with the two at the same
  // bound, core0, which polls, took the slot that came free before core1,
  // which looks once a row, and copied nearly half the rows itself.
  videoProducerMaxAhead = photo ? RING_SIZE - 4 : RING_SIZE - 3;
  prefillSrc = photo;
  __sync_synchronize();
  prefillGen = prefillGen + 1;   // after the source: core1 reads the generation first
}

void videoPrefillAspect(int bar, int active) {
  if (bar == prefillBar && active == prefillActive) return;
  prefillBar = bar;
  prefillActive = active;
  __sync_synchronize();
  prefillGen = prefillGen + 1;
}

bool videoRowPrefilled() { return rowPrefilled; }

void videoTakePrefillStats(uint32_t *byCore1, uint32_t *byCore0, uint32_t *cut, uint32_t *waits,
                           uint32_t *waitUs) {
  *byCore1 = prefillByCore1;
  *byCore0 = prefillByCore0;
  *cut = prefillCut;
  *waits = prefillWaits;
  *waitUs = prefillWaitUs;
  prefillByCore1 = prefillByCore0 = prefillCut = 0;
  prefillWaits = prefillWaitUs = 0;
}

// The picture row ring slot q carries, -1 for a blanking line: the same
// mapping as videoBuildLine() (slot k is line (k % 625) + 1).
static int __not_in_flash_func(rowOfSlot)(uint32_t q) {
  const uint32_t line = q % LINES_PER_FRAME + 1;
  const bool field2 = line >= 313;
  const uint32_t fl = field2 ? line - 312 : line;
  if (!field2 && fl >= 23 && fl <= 310) return (int)(fl - 23) * 2;
  if (field2 && fl >= 24 && fl <= 311) return (int)(fl - 24) * 2 + 1;
  return -1;
}

// Core0, at the start of row q: waits for the copy core1 is on into
// this slot, then says whether the slot holds the row.
static bool __not_in_flash_func(prefillClaim)(uint32_t q) {
  __sync_synchronize();   // videoProducedCount = q is out before prefillBusy is read
  if (prefillBusy == q) {
    // Bounded, a safety net only: core1 finishes this row first thing
    // next turn, so this is a turn and a copy at the most.
    const uint32_t t0 = time_us_32();
    while (prefillBusy == q && time_us_32() - t0 < 2000) tight_loop_contents();
    prefillWaits = prefillWaits + 1;
    prefillWaitUs = prefillWaitUs + (time_us_32() - t0);
    __sync_synchronize();
  }
  const uint32_t s = q % RING_SIZE;
  if (prefillSrc && prefillTag[s] == q && prefillTagGen[s] == (uint8_t)prefillGen) {
    prefillByCore1 = prefillByCore1 + 1;
    return true;
  }
  prefillTag[s] = PREFILL_NONE;
  if (prefillSrc) prefillByCore0 = prefillByCore0 + 1;
  return false;
}

// Core1's row in progress: where it is in the copy, from which source
// generation, and where the next chunk goes.
static uint32_t partChunk = 0, partGen = 0;
static const PrefillBlock *partSrc = nullptr;
static PrefillBlock *partDst = nullptr;

// Core1: goes on with the row it holds, chunk by chunk, until it is
// done, the channel to reload next has finished (then it waits for the
// next turn), core0 has gone past it, or the source changed under it. A
// hand-written word loop, since memcpy lives in flash.
static void __not_in_flash_func(prefillContinue)(int nextCh) {
  const uint32_t q = prefillBusy;
  if (q == PREFILL_NONE) return;
  if ((int32_t)(videoProducedCount - q) > 0 || partGen != prefillGen) {
    __sync_synchronize();
    prefillBusy = PREFILL_NONE;   // let go without a tag: core0 copies it itself
    return;
  }
  for (; partChunk < PREFILL_CHUNKS; partChunk++) {
    if (!dma_channel_is_busy(nextCh)) {
      prefillCut = prefillCut + 1;
      return;
    }
    for (uint32_t i = 0; i < PREFILL_CHUNK_WORDS / 4; i++) *partDst++ = *partSrc++;
  }
  __sync_synchronize();   // the row is in before the tag says so
  prefillTag[q % RING_SIZE] = q;
  prefillTagGen[q % RING_SIZE] = (uint8_t)partGen;
  __sync_synchronize();
  prefillBusy = PREFILL_NONE;
  prefillCursor = q + 1;
}

// Core1, first thing after a reload: the row core0 is waiting for, if
// it is waiting for one.
void __not_in_flash_func(videoPrefillUrgent)(int nextCh) {
  const uint32_t q = prefillBusy;
  if (q != PREFILL_NONE && videoProducedCount == q) prefillContinue(nextCh);
}

// Core1, with what is left of a turn: the row in progress, then new
// rows when mayStart, until the channel to reload next has finished its
// row.
void __not_in_flash_func(videoPrefillStep)(int nextCh, bool mayStart) {
  for (;;) {
    prefillContinue(nextCh);
    if (prefillBusy != PREFILL_NONE) return;   // cut by the reload: next turn
    if (!mayStart || !dma_channel_is_busy(nextCh)) return;
    // Generation first, then what it covers; videoSetPrefill() writes
    // them the other way round, so a row tagged with a generation was
    // copied from that generation's source.
    const uint32_t gen = prefillGen;
    __sync_synchronize();
    const uint8_t *src = prefillSrc;
    if (!src) return;
    const int bar = prefillBar, act = prefillActive;
    const uint32_t produced = videoProducedCount;
    if ((int32_t)(prefillCursor - produced) < PREFILL_LEAD) prefillCursor = produced + PREFILL_LEAD;
    // The same distance core0 keeps from the streamer: two rows are
    // always under way. And no further than a ring ahead of core0 either,
    // or, with core0 behind the streamer, the slots would be filled over
    // again before core0 got to them.
    if ((int32_t)(prefillCursor - videoConsumedCount) >= (int32_t)(RING_SIZE - 3) ||
        (int32_t)(prefillCursor - produced) >= (int32_t)(RING_SIZE - 3))
      return;
    const uint32_t q = prefillCursor;
    int y = rowOfSlot(q);
    if (y >= 0 && act < SCREEN_H) y = (y < bar || y >= bar + act) ? -1 : (y - bar) * SCREEN_H / act;
    if (y < 0) {
      prefillCursor = q + 1;   // blanking, or a letterbox bar: nothing to copy
      continue;
    }
    prefillBusy = q;
    __sync_synchronize();
    if ((int32_t)(videoProducedCount + PREFILL_LEAD - q) > 0) {
      prefillBusy = PREFILL_NONE;   // core0 got there meanwhile
      continue;
    }
    if (PREFILL_NOALLOC) src = (const uint8_t *)((uintptr_t)src - XIP_BASE + XIP_NOCACHE_NOALLOC_BASE);
    partChunk = 0;
    partGen = gen;
    partDst = (PrefillBlock *)(videoRing[q % RING_SIZE] + 4);   // past the SAV
    partSrc = (const PrefillBlock *)(src + (uint32_t)y * ACTIVE_SAMPLES);
  }
}

// Which of the two PAL phase frames a frame starts with, for a card that
// stores both (PM5644). Set once per frame from the encoder's field
// counter by videoSetPhaseFrame() in the frame hook; videoPhaseRow()
// gives field 2 the other one.
static bool videoPhaseFrame = false;
void videoSetPhaseFrame(bool on) { videoPhaseFrame = on; }

static bool videoRowPhase = false;
bool videoPhaseRow() { return videoRowPhase; }
void videoSetPhaseRow(bool on) { videoRowPhase = on; }

// Fills buf with one complete BT.656 line (SAV, active picture, EAV,
// blanking). line runs from 1 through 625.
//
// In RAM, like the streamer: the RP2350 fetches flash code through a
// 16 kB two-way cache, so three hot routines whose addresses agree
// modulo 8 kB evict each other on every row. Adding a card moved this,
// pcm1808Poll() and the Ham PCM row to the same cache sets, and every
// row of every card got 2 to 16 us slower with not one instruction
// changed (measured: 41 to 57 us on Ham PCM, min 21 to 23). The two
// small hot routines live in RAM now so the layout cannot do that
// again.
void __not_in_flash_func(videoBuildLine)(uint8_t *buf, uint32_t line) {
  bool field2 = (line >= 313);
  uint32_t fieldLine = field2 ? (line - 312) : line;  // 1-based within the field
  bool F = field2;
  bool V;
  bool active;
  int activeY;  // 0..287 within the active part of this field

  if (!field2) {
    V = (fieldLine <= 22) || (fieldLine >= 311);
    active = (fieldLine >= 23) && (fieldLine <= 310);
    activeY = fieldLine - 23;
  } else {
    V = (fieldLine <= 23) || (fieldLine >= 312);
    active = (fieldLine >= 24) && (fieldLine <= 311);
    activeY = fieldLine - 24;
  }

  uint8_t *p = buf;

  *p++ = 0xFF; *p++ = 0x00; *p++ = 0x00; *p++ = eavSavByte(F, V, 0);  // SAV

  if (active) {
    int y = activeY * 2 + (field2 ? 1 : 0);  // interlaced row within 0..575
    videoRowPhase = videoPhaseFrame != field2;
    rowPrefilled = prefillClaim(videoProducedCount);
    patternRenderRow(p, y);
    rowPrefilled = false;
    if (!activePattern->keepEdges) blankEdges(p);
    displaySampleRow(p, y, videoDisplaySample);   // must run even with no time to spare
    p += ACTIVE_SAMPLES;
  } else if (activePattern->vbiLine && activePattern->vbiLine(p, line)) {
    p += ACTIVE_SAMPLES;   // the card's own blanking-line data comes first
  } else if (!activePattern->rawSignal && itsRenderLine(p, line)) {
    p += ACTIVE_SAMPLES;   // ITS takes precedence: its line numbers are fixed (ITU-T J.63)
  } else if (!activePattern->rawSignal && teletextRenderLine(p, line)) {
    p += ACTIVE_SAMPLES;
  } else {
    uint32_t *pw = (uint32_t *)p;
    for (uint32_t i = 0; i < ACTIVE_SAMPLES / 4; i++) *pw++ = 0x10801080u;
    p += ACTIVE_SAMPLES;
  }

  *p++ = 0xFF; *p++ = 0x00; *p++ = 0x00; *p++ = eavSavByte(F, V, 1);  // EAV

  uint32_t *pw = (uint32_t *)p;
  for (uint32_t i = 0; i < BLANK_FILL_SAMPLES / 4; i++) *pw++ = 0x10801080u;
}

// Writes this line number's F and V into a blank row's SAV and EAV; same
// field layout as videoBuildLine(), V = !active. IN RAM: called while
// core0 writes flash.
static void __not_in_flash_func(blankLineFlags)(uint8_t *row, uint32_t line) {
  bool field2 = (line >= 313);
  uint32_t fieldLine = field2 ? (line - 312) : line;
  bool V = field2 ? ((fieldLine <= 23) || (fieldLine >= 312))
                  : ((fieldLine <= 22) || (fieldLine >= 311));
  row[3] = eavSavByte(field2, V, 0);
  row[4 + ACTIVE_SAMPLES + 3] = eavSavByte(field2, V, 1);
}

static void buildBlank(uint8_t *buf, bool F, bool V) {
  uint8_t *p = buf;
  *p++ = 0xFF; *p++ = 0x00; *p++ = 0x00; *p++ = eavSavByte(F, V, 0);  // SAV
  uint32_t *pw = (uint32_t *)p;
  for (uint32_t i = 0; i < ACTIVE_SAMPLES / 4; i++) *pw++ = 0x10801080u;
  p += ACTIVE_SAMPLES;
  *p++ = 0xFF; *p++ = 0x00; *p++ = 0x00; *p++ = eavSavByte(F, V, 1);  // EAV
  pw = (uint32_t *)p;
  for (uint32_t i = 0; i < BLANK_FILL_SAMPLES / 4; i++) *pw++ = 0x10801080u;
}

static void buildBlankLines() {
  for (int i = 0; i < 2; i++) buildBlank(blankLine[i], false, false);
}

void videoRequestSflPulse() {
  if (sflPhase == 0) sflPhase = 1;
}

bool videoSflPulseBusy() { return sflPhase != 0; }

void videoAbortSflPulse() {
  sflPhase = 0;
  gpio_put(PIN_SFL, 0);
}

// ---------------------------------------------------------------------
// Writing to flash without stopping the video chain.
//
// Flash cannot be programmed while code runs out of XIP. The streamer
// loop lives entirely in RAM and keeps running while core0 erases and
// programs; RP2350 datasheet page 388 (5.4.8.10 flash_range_erase):
//
//   "For the duration of the erase operation, QMI is in direct mode and
//    attempting to access XIP from DMA, the debugger or the other core
//    will return a bus fault. XIP becomes accessible again once the
//    function returns."
//
// Core1's instructions, the ring buffer and the blank rows are all in
// RAM, and the DMA only reads RAM, so nothing here touches XIP. The PIO
// and DMA are untouched too, so CLKIN and the line structure never stop
// and no SFL timing reset is needed afterwards.
//
// Core1's interrupts go off before any of this (a flash-resident
// handler would itself read flash while it is being erased/written;
// see pico/flash.h), and core0 waits for core1 to confirm it is in that
// loop before the first erase goes out.
volatile bool videoParkRequest = false;
volatile bool videoParked = false;
volatile bool videoRestartRequest = false;

bool videoSuspend() {
  videoParkRequest = true;
  // Core1 checks the request once per row (64 us); 200 ms is ample.
  uint32_t t0 = time_us_32();
  while (!videoParked && (time_us_32() - t0) < 200000) tight_loop_contents();
  if (!videoParked) {
    videoParkRequest = false;
    return false;
  }
  return true;
}

bool videoIsSuspended() { return videoParked; }

// Hooks the producer onto the streamer's line counter: ring slot k
// belongs to line (k % 625) + 1, videoConsumedCount is rows actually
// sent. Levels videoProducedCount with videoConsumedCount rather than
// ahead of it: overstating it would let the streamer send an unbuilt
// row.
static void hookProducerOn() {
  videoProducedCount = videoConsumedCount;
  videoRestartRequest = true;
}

void videoResume() {
  hookProducerOn();
  videoParkRequest = false;
  // Wait until core1 has really left the parked loop, or a second
  // videoSuspend() right after could start erasing too early.
  uint32_t t0 = time_us_32();
  while (videoParked && (time_us_32() - t0) < 200000) tight_loop_contents();
}

static void configureDma() {
  for (int i = 0; i < 2; i++) {
    int ch = i ? videoDmaChanB : videoDmaChanA;
    int other = i ? videoDmaChanA : videoDmaChanB;
    dma_channel_config dc = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, pioSm, true));
#if CFG_PIXEL_BUS_REVERSED
    // Half of the mirroring: byte-swap each word here, bit-reverse the
    // whole word in the PIO (see bt656_out_rev in bt656.pio).
    channel_config_set_bswap(&dc, true);
#endif
    channel_config_set_chain_to(&dc, other);
    dma_channel_configure(ch, &dc, &pio->txf[pioSm], nullptr, WORDS_PER_LINE, false);
  }
}

void videoStartPio() {
  buildBlankLines();

#if CFG_PIXEL_BUS_REVERSED
  uint offset = pio_add_program(pio, &bt656_out_rev_program);
  pio_sm_config c = bt656_out_rev_program_get_default_config(offset);
  // Explicit pull (the program bit-reverses the OSR after it), so no
  // autopull; a stall on an empty FIFO still sets TXSTALL either way.
  const bool autopull = false;
  const uint startPc = offset + bt656_out_rev_offset_entry;
#else
  uint offset = pio_add_program(pio, &bt656_out_program);
  pio_sm_config c = bt656_out_program_get_default_config(offset);
  const bool autopull = true;
  const uint startPc = offset;
#endif

  for (int i = 0; i < 8; i++) pio_gpio_init(pio, DATA_PIN_BASE + i);
  pio_gpio_init(pio, CLKOUT_PIN);

  sm_config_set_out_pins(&c, DATA_PIN_BASE, 8);
  sm_config_set_sideset_pins(&c, CLKOUT_PIN);
  sm_config_set_out_shift(&c, true, autopull, 32);  // shift right, 32-bit
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

  sm_config_set_clkdiv_int_frac(&c, PIO_CLKDIV, 0);

  pio_sm_set_consecutive_pindirs(pio, pioSm, DATA_PIN_BASE, 8, true);
  pio_sm_set_consecutive_pindirs(pio, pioSm, CLKOUT_PIN, 1, true);

  pio_sm_init(pio, pioSm, startPc, &c);
  pio_sm_set_enabled(pio, pioSm, true);

  // Two channels chained into each other so the PIO FIFO (8 words =
  // 1.2 us) never runs empty between rows: chain_to starts the next
  // channel immediately, core1 only has to reload the one that just
  // finished, with a full line time to do it.
  videoDmaChanA = dma_claim_unused_channel(true);
  videoDmaChanB = dma_claim_unused_channel(true);
  configureDma();
}

// ---------------------------------------------------------------------
// The streamer. Entirely in RAM: __not_in_flash_func, and everything it
// calls (dma_*, pio_*, gpio_put, tight_loop_contents) is static inline
// in the pico-sdk so it comes along too.
//
// Check after every change, with arm-none-eabi-objdump -d on the elf:
// loop1, videoLoadLine, blankLineFlags, videoKickChain, videoSflStep and
// videoCountTxStall must all sit at an address in 0x2000_0000.., with no
// bl branch out of them going to 0x1000_0000..
//
// ---------------------------------------------------------------------
// The chain is never stopped, and that is a rule: after videoStartPio()
// no path touches chain_to, CTRL, or the PIO/DMA configuration again,
// and no channel is ever aborted.
//
// Anyone who ever does need an abort here must follow the RP2350
// datasheet's procedure (page 1108, 12.6.8.3 Channel abort):
//
//   "1. Clear the EN bit and disable CHAIN_TO for all channels to be
//       aborted.
//    2. Write the CHAN_ABORT register with a bitmap of those same
//       channels.
//    3. Poll the ABORT register until all bits set by the previous
//       write are clear."
//
// and page 1137, register CHAN_ABORT: "After writing, this register
// must be polled until it returns all-zero. Until this point, it is
// unsafe to restart the channel."
//
// Step 1 is also the workaround erratum RP2350-E5 prescribes: "Before
// aborting an active channel, clear the EN bit of both the aborted
// channel and any channel it chains to. This ensures the channel isn't
// susceptible to re-triggering." For a chain, page 1108 adds: "When
// aborting a channel involved in a CHAIN_TO, it is recommended to
// simultaneously abort all other channels involved in the chain."
//
// Do not use dma_channel_abort() from the pico-sdk for it: it waits on
// the BUSY flag, not the ABORT register --
//
//     dma_hw->abort = 1u << channel;
//     while (dma_hw->ch[channel].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS)
//         tight_loop_contents();
//
// (pico-sdk, hardware_dma/include/hardware/dma.h) -- and its own doc
// comment mentions RP2350-E5 without carrying out the workaround, so by
// the datasheet the channel may not yet be safe to restart.
//
// The watchdog below stays as a safety net that should never fire.

static bool producerRunning = false;   // suppresses false starves during bring-up

static const uint32_t LOAD_STALL_US = 3 * VIDEO_LINE_BUDGET_US;

// Loads one row into the given channel. `blank` forces a black row,
// used while writing to flash.
void __not_in_flash_func(videoLoadLine)(int ch, bool blank) {
  // Measures, does not limit: dma_channel_set_read_addr()/
  // dma_channel_set_trans_count() below may only be written once the
  // channel has stopped being busy (the SDK's own reload contract), so
  // breaking out early is not safe. videoFeedWatchdog() covers the case
  // where this never returns at all.
  const uint32_t t0 = time_us_32();
  while (dma_channel_is_busy(ch)) tight_loop_contents();
  const uint32_t waited = time_us_32() - t0;
  videoCore1WaitUs += waited;
  if (waited > LOAD_STALL_US) loadStallCount++;

  const uint8_t *bron;
  // The wrap guard's second half; see VIDEO_RENORM_STEP. First thing,
  // so at most the one row already dispatched between core0's request
  // and this call goes out blank.
  if (videoRenormRequest) {
    videoConsumedCount -= VIDEO_RENORM_STEP;
    prefillCursor -= VIDEO_RENORM_STEP;
    videoRenormRequest = false;
  }

  // Signed compare: after videoResume() the producer can be briefly
  // behind, and an unsigned compare would flip that to "well ahead".
  if (!blank && (int32_t)(videoProducedCount - videoConsumedCount) > 0) {
    bron = videoRing[videoConsumedCount % RING_SIZE];
    producerRunning = true;
  } else {
    if (blank) {
      flashLineCount++;
    } else if (producerRunning) {
      starveCount++;
    }
    uint8_t *b = blankLine[ch == videoDmaChanA ? 0 : 1];
    blankLineFlags(b, (videoConsumedCount % LINES_PER_FRAME) + 1);
    bron = b;
  }
  dma_channel_set_read_addr(ch, bron, false);
  dma_channel_set_trans_count(ch, WORDS_PER_LINE, false);
  videoConsumedCount++;
}

// Safety net for erratum RP2350-E8 ("CHAIN_TO might not fire for
// zero-length transfers"): if the sending channel finishes before core1
// has reloaded the other, chaining can stall. Kept with a counter so it
// shows up if it ever fires.
void __not_in_flash_func(videoKickChain)(int ch) {
  if (!dma_channel_is_busy(videoDmaChanA) && !dma_channel_is_busy(videoDmaChanB)) {
    dma_channel_start(ch);
    chainRestartCount++;
  }
}

void __not_in_flash_func(videoSflStep)() {
  if (!sflPhase) return;
  // Two rows are always under way: the one just loaded and the one sending.
  uint32_t line = ((videoConsumedCount - 2) % LINES_PER_FRAME) + 1;
  if (sflPhase == 1 && line == LINES_PER_FRAME) {
    gpio_put(PIN_SFL, 1);   // counters go into reset and stay there
    sflPhase = 2;
  } else if (sflPhase == 2 && line == 1) {
    gpio_put(PIN_SFL, 0);   // release: resume at field 1
    sflPhase = 0;
  }
}

static bool txStallFirst = true;
void __not_in_flash_func(videoCountTxStall)() {
  if (txStallFirst) {
    // The PIO was already waiting for its first data when switched on;
    // that stall is normal.
    pio->fdebug = TXSTALL_BIT;
    txStallFirst = false;
    return;
  }
  if (pio->fdebug & TXSTALL_BIT) {
    pio->fdebug = TXSTALL_BIT;
    txStallCount++;
  }
}

void __not_in_flash_func(videoStreamBegin)() {
  // Load both channels and start the chain: with nothing in the ring
  // buffer yet these become two blank rows, so CLKIN is already running
  // during encoder bring-up, and this chain is never stopped again.
  videoConsumedCount = 0;
  videoLoadLine(videoDmaChanA, true);
  videoLoadLine(videoDmaChanB, true);
  dma_channel_start(videoDmaChanA);

  clockReady = true;   // only here: core0 waits on this before resetting the encoder
}

void videoProducerBegin() {
  videoSetPrefill(nullptr);   // a photograph's init() sets it
  activePattern->init();
  hookProducerOn();
}

static uint32_t wdtLastConsumed = 0;
void videoFeedWatchdog() {
  const uint32_t now = videoConsumedCount;
  if (now == wdtLastConsumed) return;
  wdtLastConsumed = now;
  rp2040.wdt_reset();
}

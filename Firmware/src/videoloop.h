// The innards of video.cpp that the two loops in Firmware.ino need.
//
// loop() and loop1() are the producer and the streamer; they run on the
// ring buffer, its two counters, the two DMA channels and the handshake
// that parks core1 while core0 writes to flash. A normal caller needs
// only video.h.
#ifndef VIDEOLOOP_H
#define VIDEOLOOP_H

#include <stdint.h>

// --- the raster -------------------------------------------------------

static const uint32_t VIDEO_SAMPLES_PER_LINE = 1728;
static const uint32_t VIDEO_LINES_PER_FRAME = 625;

// aligned(4): the filling functions write 32 bit words and 1728 divides
// by 4, so with an aligned base every row is aligned.
//
// 62, down from 64, to give 3.5 kB of RAM back to the Sony PCM card's
// interleave and resampler when the firmware sat within a few hundred
// bytes of the linker's limit. The producer runs at most
// VIDEO_RING_SIZE - 3 = 59 rows ahead, still above every threshold
// below (DISPLAY_KEEP 52 leaves 7 rows, was 9).
static const uint32_t VIDEO_RING_SIZE = 62;

// Thresholds on the producer's row stock: below PANIC a card change
// finishes copying its tables in one go instead of pieces; above KEEP
// it may copy; above DISPLAY_KEEP the status display may transfer. See
// video.cpp.
static const int32_t VIDEO_ROM_COPY_PANIC_ROWS = 8;
static const int32_t VIDEO_ROM_COPY_KEEP_ROWS = 44;
static const int32_t VIDEO_DISPLAY_KEEP_ROWS = 52;

static const uint32_t VIDEO_ROM_COPY_CHUNK = 4096;
static const uint32_t VIDEO_DISPLAY_CHUNK = 512;

// --- what the producer works on ---------------------------------------

extern uint8_t videoRing[VIDEO_RING_SIZE][VIDEO_SAMPLES_PER_LINE];
extern volatile uint32_t videoProducedCount;
extern volatile uint32_t videoConsumedCount;

// The wrap guard for the two free-running row counters. Every mapping
// derived from them (ring slot = counter % 62, line = counter % 625)
// silently assumes they never wrap, but 2^32 % 62 = 4 and 2^32 % 625 =
// 421: at the wrap, after 76 h of streaming, ring slots collided and
// the blank rows' field flags jumped mid-frame. Long before that, both
// counters step down together by this multiple of lcm(62, 625) = 38750
// (and < 2^31, so the one-row skew reads as a negative stock, never a
// positive one): core0 lowers videoProducedCount and raises the
// request, core1 lowers videoConsumedCount at its next row. Every
// modulo mapping and every difference is preserved exactly; the one
// row in between sees an empty stock and goes out blank.
static const uint32_t VIDEO_RENORM_STEP = 38750u * 48491u;
extern volatile bool videoRenormRequest;
extern volatile uint32_t videoStallCount;
extern volatile bool videoRestartRequest;

extern bool videoDisplayMayWork;
extern bool videoDisplaySample;

// The timing report, filled in by the producer and read by key t.
extern volatile uint32_t videoWorstBuildUs;
extern volatile uint32_t videoMinBuildUs;
extern volatile uint64_t videoSumBuildUs;
extern volatile uint32_t videoSampleCount;
extern volatile uint32_t videoOverBudgetCount;

void videoBuildLine(uint8_t *buf, uint32_t line);

// Sets up the pattern and hooks on to the streamer. Call once, from
// setup(), before the loop starts turning.
void videoProducerBegin();

// --- what the streamer works on ---------------------------------------
//
// Everything here lives in RAM: core1 may not touch XIP while core0
// writes to flash, so this way it simply carries on. loop1() in the
// sketch therefore also needs __not_in_flash_func.

extern int videoDmaChanA, videoDmaChanB;

// The handshake that parks core1 while core0 writes to flash. core0 sets
// the request and waits for parked; core1 only sets parked from inside
// its RAM loop, so an erase can never start while core1 is in flash.
extern volatile bool videoParkRequest;
extern volatile bool videoParked;

void videoLoadLine(int ch, bool blank);
void videoKickChain(int ch);
void videoSflStep();
void videoCountTxStall();

// Loads both channels and starts the chain. Call once, from setup1(),
// before the loop starts turning.
void videoStreamBegin();
#endif  // VIDEOLOOP_H

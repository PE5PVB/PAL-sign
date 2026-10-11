// The video layer: turns the active test card into a BT.656 stream and
// clocks it out to the ADV7391. Knows nothing about which pattern is
// being drawn.
//
// core0's loop() in Firmware.ino renders rows into a ring buffer; core1's
// loop1() feeds the DMA with finished rows.
#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>

// PAL 625 lines, 8 bit 4:2:2 YCbCr, embedded EAV/SAV. At 27 MHz that is
// 1728 samples per line, so one line lasts 64 us.
static const uint32_t VIDEO_LINE_BUDGET_US = 64;

struct VideoStats {
  uint32_t minUs;       // fastest row build since the start
  uint32_t avgUs;       // average row build
  uint32_t worstUs;     // slowest row build since the start
  uint32_t overBudget;  // rows that took longer than the budget
  uint32_t lines;       // total rows built
  uint32_t starves;     // ring buffer was empty, core1 had to wait
  uint32_t txStalls;    // the PIO ran out of data
  uint32_t chainRestarts;   // the DMA chain stopped and was restarted
  uint32_t producerStalls;  // core0 gave up waiting for core1
  uint32_t flashLines;      // blank rows sent while writing flash
  uint32_t loadStalls;      // videoLoadLine()'s busy-wait ran unusually long
};

// Sets up the PIO and the two chained DMA channels on core1, and builds
// the two blank rows in RAM.
void videoStartPio();

// Requests an SFL timing reset: aligns the encoder's field 1 with ours.
// Put the encoder in timing reset mode with advSetTimingReset(true)
// first; see videoSflPulseBusy().
void videoRequestSflPulse();
bool videoSflPulseBusy();

// Aborts a stuck pulse and puts the pin low.
void videoAbortSflPulse();

// Parks core1 in RAM with interrupts off so core0 can erase/program
// flash safely. Call once around a whole run of sectors. Returns false
// if core1 did not confirm in time; do not write to flash in that case.
bool videoSuspend();
void videoResume();
bool videoIsSuspended();

// Is the pixel clock running? Core0 waits on this before resetting the
// encoder.
bool videoClockReady();

void videoGetStats(VideoStats *s);

// Called once per frame from the producer loop, in vertical blanking.
void videoFrameHook();

// Selects the PAL phase frame field 1 of the coming frame starts with,
// for a Pattern::phaseFrames card; set once per frame from the encoder's
// field counter.
void videoSetPhaseFrame(bool on);

// The phase frame of the row being built, for a Pattern::phaseFrames
// card: the frame's own in field 1, the other one in field 2. The
// PM5644 plays its EPROM fields in the order field 2, field 1, so one
// generator frame is field 2 of a picture plus field 1 of the next.
bool videoPhaseRow();

// Sets it for a row rendered outside videoBuildLine() (the card dump
// asks for phase frame 0); the next video row sets it again.
void videoSetPhaseRow(bool on);

// Kicks the hardware watchdog, only when core1 has made progress since
// the last kick. Safe to call from anywhere on core0.
void videoFeedWatchdog();

// A photograph's rows copied into the ring ahead by core1; see the
// prefill section in video.cpp. The source is the picture's row 0 in
// XIP, or nullptr for a card that draws its own rows; a photograph's
// init() sets it, and it is cleared before every init().
void videoSetPrefill(const uint8_t *photo);

// The letterbox the prefill applies, as aspect.cpp applies it to
// renderRow(): rows outside [bar, bar + active) are black, the rest
// squeezed by skipping rows.
void videoPrefillAspect(int bar, int active);

// True while the row being built already holds its picture: the card's
// renderRow() may then return at once.
bool videoRowPrefilled();

// For the timing report: rows core1 copied in, rows core0 copied itself
// (core1 was not on them), copies a reload cut short (taken up again),
// and how often and how long core0 waited for a row core1 was on.
void videoTakePrefillStats(uint32_t *byCore1, uint32_t *byCore0, uint32_t *cut, uint32_t *waits,
                           uint32_t *waitUs);
#endif  // VIDEO_H

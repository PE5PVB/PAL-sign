// PCM1808 experiment: a system clock for the ADC on GP13 (PIN_PCM_SCKI,
// see board.h) and an I2S receiver on GP9-11, both via pio1. Not part
// of any normal card path; only PATTERN_PCMAUDIO in testcards/ calls
// into this.
#ifndef PCM1808_H
#define PCM1808_H

#include <stdint.h>

// Starts the SCKI clock and the I2S receiver. Call once from setup(),
// after the system clock is at CFG_SYSCLK_HZ.
void pcm1808Begin();

// Drains whatever the DMA ring has captured since the last call (one
// 32 bit word per channel, framed by the PIO) into stereo pairs. Cheap:
// a handful of words a row. Call once a row, from the PCM audio card's
// renderRow(), on every row including the ones that carry no audio.
void pcm1808Poll();

// Takes up to `max` decoded stereo pairs, oldest first, 16 bit each
// (the low 8 of the ADC's 24 are dropped). Returns how many were
// actually available, which can be fewer than `max` or zero.
int pcm1808TakePairs(int16_t *l, int16_t *r, int max);

// How many decoded pairs are waiting: the steering signal for the
// sample-rate converter (asrc.cpp).
int pcm1808Available();

// Whether the boot probe saw an ADC actually clocking (pcm1808Begin());
// the PCM cards hide completely when it did not.
bool pcm1808Present();

// Drops everything queued; a PCM card calls it in init so stale audio
// from the last time a PCM card showed is not replayed.
void pcm1808Flush();

#endif  // PCM1808_H

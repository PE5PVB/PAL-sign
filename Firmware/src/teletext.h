// Teletext (World System Teletext) in the vertical blanking. The data
// waveform goes into the luma samples of the VBI lines ourselves; the
// ADV7391 has no teletext inserter and needs its VBI pass-through on
// (register 0x83 bit 4, see advSetVbiOpen()) or the blanking is blanked
// away entirely.
//
// Signal timing per ETS 300 706 and ITU-R BT.653-3/BT.601-5; the
// working from those numbers to sample positions is in teletext.cpp.
// Everything comes out on whole numbers because 13.5 MHz / 6.9375
// Mbit/s is exactly 72/37.
#ifndef TELETEXT_H
#define TELETEXT_H

#include <stdint.h>

// Text rows X/1..X/TT_ROWS plus the header X/0. 15+1 = 16 packets, the
// exact line count PAL field 1 offers (7..22), so the whole page fits
// in one field and repeats identically in the other.
static const int TT_ROWS = 15;
static const int TT_PACKETS = TT_ROWS + 1;

// Characters per line: 40 in the row packets, 32 in the header.
static const int TT_COLS = 40;
static const int TT_HEADER_COLS = 32;

// Builds the pulse table and works out every packet once. Call after
// clockSet() and before the producer loop runs.
void teletextInit();

bool teletextOn();
void teletextSetOn(bool on);

// Call once per picture from videoFrameHook(); the header waveform only
// recomputes when the second ticks over.
void teletextTick();

// Fills 720 luma samples (1440 bytes, 4:2:2, chroma neutral) with the
// teletext line for picture line `line` (1..625). Returns false when
// there is none, in which case the caller fills black itself.
bool teletextRenderLine(uint8_t *uyvy, uint32_t line);

// Sets the text of a row (1..TT_ROWS); Latin G0 only, no national
// positions.
void teletextSetRow(int row, const char *text);

// Applies row `row` (1..TT_ROWS) from cfgTeletextRow(); an empty stored
// row (never customised) falls back to the built-in default page.
void teletextApplyRow(int row);

// Rebuilds the transmitted waveform of one edited row; without it an
// edit only shows after a restart.
void teletextRerasterRow(int row);

// The row (1..TT_ROWS) as it is actually being sent right now, trailing
// spaces trimmed, whether that came from a stored setting or the
// built-in default page. For the PC tool's editor, which opens on the
// real current page rather than on the raw (possibly empty) setting.
void teletextRowText(int row, char *buf, unsigned n);

// Fewer than TT_PACKETS when ITS claims lines from the blanking; see its.h.
int teletextPacketsPerField();

void teletextStatus(char *buf, unsigned n);
#endif  // TELETEXT_H

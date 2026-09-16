// Insertion test signals in the vertical blanking interval: four
// measurement lines to ITU-T J.63 (formerly ITU-R CMTT.473, annex I),
// showing amplitude response, group delay and linearity on a scope
// without the picture having to give way.
//
//   line  17   bar, 2T pulse, composite 20T pulse, five riser staircase
//   line  18   pedestal, reference bar, multiburst 0.5 to 5.8 MHz
//   line 330   as 17, but with the staircase under colour subcarrier
//   line 331   chroma bar and reference subcarrier
//
// Computed by tools/make_its.py, which also measures the result back
// against the standard; see its_data.h for the four ready-made 4:2:2
// lines (5760 bytes flash, no RAM).
//
// Needs the VBI open (register 0x83 bit 4, see advSetVbiOpen()) or
// nothing comes out. Clashes with teletext (lines 7-22, 320-335): ITS
// wins, since its line numbers are fixed by the standard while teletext
// can shift its packets, so teletext skips these four lines and the
// last two text rows do not go out.
#ifndef ITS_H
#define ITS_H

#include <stdint.h>

static const int ITS_COUNT = 4;   // see ITS_LINES in its_data.h for the picture lines

void itsSetEnabled(bool on);
bool itsEnabled();

// Does ITS claim this picture line? Teletext uses this to lay its
// packets out around them.
bool itsClaimsLine(uint32_t line);

// Fills the active part of picture line `line`. Returns false (buffer
// untouched) if this is not an ITS line or ITS is off.
bool itsRenderLine(uint8_t *uyvy, uint32_t line);

void itsStatus(char *buf, unsigned n);
#endif  // ITS_H

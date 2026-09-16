// Sample-rate converter from the PCM1808's 48 kHz to the exact 44.1 kHz
// an EIAJ/Sony PCM line structure needs (3 stereo samples per data
// line, 294 data lines per field, 50 fields: 44100 exactly, locked to
// the video). 44.1 kHz is not derivable from the 108 MHz system clock,
// so the ADC keeps its own rate and this bridges the two: a polyphase
// FIR interpolator (src/asrc_taps.h, tools/make_asrc.py) with a
// conversion ratio steered by the fill level of the input, the same
// idea as PcmDecoder's AdaptiveResampler on the PC.
#ifndef ASRC_H
#define ASRC_H

#include <stdint.h>

// Forgets all history and starts from the nominal ratio for `outHz`:
// 44100 (the Sony/EIAJ card, steered by asrcSteer()), 32000 (Ham PCM
// LQ and Voice, an exact 3:2) or 16000 (Voice narrow, 3:1), the two
// last never steered.
void asrcReset(int outHz);

// Output pairs the input queue can cover right now, for a caller that
// sends what is there rather than a fixed count.
int asrcAvailable();

// Produces `n` output pairs at the converted rate, pulling input from
// pcm1808TakePairs() as needed. If the input runs dry the last pair is
// held, so the caller always gets `n` pairs.
void asrcPull(int16_t *l, int16_t *r, int n);

// Once per field: nudges the ratio so the input queue neither drains
// nor overflows. Cheap.
void asrcSteer();

#endif  // ASRC_H

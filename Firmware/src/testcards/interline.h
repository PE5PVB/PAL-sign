// A line of Inter (Mixed bars' face) scaled to fit a box: laid out from a
// 110 row run face (BATC_FONT) and scaled down by area once, so the
// letters keep their smooth round edges at any size. Shared by the BATC
// and BBC Micro cards; the face lives in flash once.
#ifndef INTERLINE_H
#define INTERLINE_H

#include <stdint.h>

#include "../gfx.h"

// The ink as lookup tables from coverage (0..255) to Y, Cb, Cr over black.
struct InterInk {
  YCbCr ink;
  uint8_t y[256], cb[256], cr[256];
};
void interInkBuild(InterInk *k, YCbCr ink);

// Lays s out and scales it into cov (w x h bytes, row by row), as large as
// fits boxW x boxH, never enlarged; squeeze below 1 makes the letters that
// much narrower than the face. The memory after the map, up to end, is
// scratch for the scaling. False when there is nothing to letter or no
// room; w and h are then 0.
bool interLineLayout(const char *s, int boxW, int boxH, uint8_t *cov, uint8_t *end, int *w,
                     int *h, float squeeze = 1.0f);

// One coverage row into a picture row from x.
void interDrawRow(uint8_t *base, int x, const uint8_t *cov, int w, const InterInk *k);
#endif  // INTERLINE_H

// A moving line: a white vertical stripe that sweeps corner to corner
// through a black box of the test card, one second each way.
//
// A card declares the feature by calling movingLineBox() from its tick,
// same as the insert boxes (inserts.h); on/off is a per-card setting
// (settings.h). The box must carry neutral chroma: the stripe writes
// luma only, blending towards peak white.
#ifndef MOVINGLINE_H
#define MOVINGLINE_H

#include <stdint.h>

// The box is [x0,x1) x [y0,y1) in picture coordinates. Call from the
// pattern's tick.
void movingLineBox(int x0, int x1, int y0, int y1);

// Until the new card's tick has run, assume it has no box. Called at
// every card change, next to insertClearInUse().
void movingLineClearInUse();
bool movingLineInUse();

// The box of the card on screen; meaningful only once movingLineInUse()
// is true. Sent in the card dump so the PC tool can animate the stripe.
void movingLineGetBox(int *x0, int *x1, int *y0, int *y1);

// Advance one step. Once per picture, from videoFrameHook().
void movingLineFrame();

// Leaves the stripe out while the PC tool fetches a picture.
void movingLinePause(bool v);

// Draws the stripe on row y. Call at the END of renderRow, so the
// stripe passes over the text. Safe to call for every row.
void movingLineDrawRow(uint8_t *base, int y);

// Is the stripe really being drawn this picture: declared, on, not
// paused.
bool movingLineActive();
#endif  // MOVINGLINE_H

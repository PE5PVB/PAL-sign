// A ticker: one line of text scrolling right to left, in a band across
// the picture, over any test card.
//
// The band replaces the card row rather than drawing over it: it is
// opaque, so patternRenderRow() in aspect.cpp asks tickerCoversRow()
// before rendering.
//
// The step is always an even number of samples: fillSpanSolid() writes
// chroma only at an even x, so an odd offset would drift the colour
// edges from the luma edges as the text scrolls. Speed is a ladder
// (cfgTickerSpeed), every rung keeping the chroma phase fixed.
//
// The offset moves only in tickerFrame(), once per picture and not per
// field, or every vertical stroke in the text combs at 25 Hz.
#ifndef TICKER_H
#define TICKER_H

#include <stdint.h>

static const int TICKER_MAX_CHARS = 96;

// Prepares the layout: looks the glyphs up, works out the positions and
// the width of one turn. Call after any change to the text or the font.
void tickerBegin();

// Advance one step. Once per picture, from videoFrameHook().
void tickerFrame();

// Holds the band off while a card is being copied: the copy only runs
// while the ring buffer is above its threshold, and the band is what
// pulls it down. Safe to call for every row.
void tickerPause(bool v);

// An uploaded photograph has no room left in its row budget for the
// band on top. Set at every card change so transparent falls back to
// solid rather than a stale setting bringing the picture down.
void tickerAllowTransparent(bool v);

// Does row y have to be drawn as a bar INSTEAD of the card? Only true
// in the solid mode.
bool tickerSolidRow(int y);

void tickerRenderRow(uint8_t *base, int y);

// Puts the text over a row the card has already drawn; does nothing
// unless the mode is transparent and the row is in the band. Draws a
// one-sample outline first, same as textDrawRowOutline() for the insert
// boxes, since white text is unreadable over a light card otherwise.
void tickerOverlayRow(uint8_t *base, int y);

// The height of the band at the font in use, padding included. Always
// even, so the edges cannot flicker at 25 Hz.
int tickerHeight();

// What the band really costs, measured: the time in tickerRenderRow()
// and tickerOverlayRow() alone, without the card underneath. Reading it
// clears it.
void tickerTakeStats(uint32_t *rows, uint32_t *sumUs, uint32_t *worstUs, uint32_t *glyphs);

enum {
  TICKER_OFF = 0,
  TICKER_SOLID,       // a bar of its own, the card row is not rendered
  TICKER_TRANSPARENT, // straight over the card, with an outline
  TICKER_MODE_COUNT
};

int tickerMode();
void tickerSetMode(int m);
const char *tickerModeName();

// The top row of the band. Rounded down to an even row and clamped so
// the band stays on the screen.
int tickerY();
void tickerSetY(int y);

// Index into FONTS[]. Changing it changes the height of the band.
int tickerFont();
void tickerSetFont(int i);
const char *tickerFontName();

const char *tickerText();
void tickerSetText(const char *s);
#endif  // TICKER_H

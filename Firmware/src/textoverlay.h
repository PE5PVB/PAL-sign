// Text overlay, independent of the pattern: give it a box, the text is
// centred in it and the letter spacing shrinks automatically when it
// would not otherwise fit.
//
// TWO FONTS, chosen with textSetFont(). See font_data.h and
// tools/make_fonts.py.
//
//   PM5544  traced from a broadcast photograph, cell 14x30. Fixed
//           width, 14 px per character except punctuation at 6 px.
//   PM8546  Nimbus Sans L Bold, 26 pt, 24 rows; approximates the real
//           PM8546, is not it. Proportional.
//
// Digits in a proportional font all get the width of the widest digit
// and sit centred in it, or a running clock jumps sideways when a 1
// replaces an 8. Vertical centring is on the real ink of the text, not
// the cell height, or the line sits visibly too high.
#ifndef TEXTOVERLAY_H
#define TEXTOVERLAY_H

#include <stdint.h>
#include "font_data.h"
#include "gfx.h"
#include "settings.h"

// The longest a stored line can ever be: CFG_TEXT_MAX minus the
// terminator.
static const int TEXT_MAX_CHARS = CFG_TEXT_MAX - 1;

// A prepared line of text: filled in once, so the render loop only has
// to draw per picture row.
struct TextOverlay {
  const Glyph *glyph[TEXT_MAX_CHARS];
  int16_t x[TEXT_MAX_CHARS];
  const Font *font;
  int count;
  int y0;
  uint8_t sx, sy;  // extra enlargement on top of the font's own
  // How many font rows really belong together, normally 1; 2 when the
  // font sits doubled in the table (make_fonts.py). Only matters for
  // enlarged text.
  uint8_t step;
};

void textSetFont(int i);
int textGetFont(void);
const char *textFontName(int i);

// Turns text into glyphs and works out the positions, centred in the
// box [bx0,bx1) x [by0,by1). Allowed: A-Z, 0-9, space, O-slash and the
// font's punctuation; lower case becomes upper case, unknown characters
// become a space.
void textPrepare(TextOverlay *t, const char *s, int bx0, int bx1, int by0, int by1);

// Enlarged: scale 1 is ordinary size, at 5 every character is drawn
// five times as high and wide. The scale is a MAXIMUM, the text comes
// down on its own if it does not fit. From scale 2 up the edges are
// followed instead of the blocks enlarged (see "following edges" in
// textoverlay.cpp), so a sloping edge stays a line, not a staircase.
void textPrepareScaled(TextOverlay *t, const char *s, int bx0, int bx1, int by0, int by1,
                       int scale);

// An outline in colour c, call before textDrawRow(). True size only;
// enlarged text already follows its own edges. Thickness in samples,
// default 1, chosen by eye per caller.
void textDrawRowOutline(uint8_t *base, const TextOverlay *t, int y, YCbCr c, int thickness = 1);

// Draws the font row belonging to picture row y, in colour c; does
// nothing outside the text, safe to call from every row. From scale 2
// up it blends the luma at span ends with what is already there and
// leaves the chroma alone, so the background must already be in place.
void textDrawRow(uint8_t *base, const TextOverlay *t, int y, YCbCr c);

// Fills row y over [x0,x1) with colour c when y falls within [y0,y1):
// a box the test card itself has none of.
void textFillBox(uint8_t *base, int x0, int x1, int y0, int y1, int y, YCbCr c);

// The ink of the shared lettering, cached in RAM (converted from the
// active card's stored colours) since it is asked for on every row.
// Firmware.ino refreshes it at every card change and colour set. The
// outlines and the black boxes under text stay black.
YCbCr textInkId();      // the first line of the identification
YCbCr textInkSub();     // the second line
YCbCr textInkInsert();  // the insert boxes: date, time, or the fixed texts
void textInkRefresh();
#endif  // TEXTOVERLAY_H

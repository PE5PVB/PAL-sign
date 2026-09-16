// The shape of a smooth font, shared by every table
// tools/make_display_font.py writes out: one alpha byte per pixel
// instead of one bit, so the renderer can blend an edge into the
// background instead of stepping diagonals and round bowls.
#ifndef SMOOTHFONT_H
#define SMOOTHFONT_H

#include <stdint.h>

struct SmoothGlyph {
  uint8_t ch;
  uint8_t w, h;    // size of the alpha bitmap
  uint8_t adv;     // how far the pen moves on
  int8_t dx;       // left side bearing
  int8_t dy;       // baseline to the top row of the bitmap
  uint32_t off;    // start in the font's pixel block
};

// A whole face, so one drawing routine serves every size. Drawing
// centres the ink band (inkUp/inkDown, how far the tallest/deepest
// glyph reaches), not the nominal ascent+descent line.
struct SmoothFont {
  const SmoothGlyph *glyphs;
  const uint8_t *pixels;
  int count;
  int ascent;    // baseline to the top of the line, as the file has it
  int descent;   // baseline to the bottom of the line, as the file has it
  int height;    // ascent + descent
  int space;     // advance of the space, which carries no ink
  int inkUp;     // baseline to the highest ink of any glyph kept
  int inkDown;   // baseline to the lowest ink of any glyph kept
};
#endif  // SMOOTHFONT_H

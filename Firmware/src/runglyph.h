// The shape of one run glyph, shared by the tables
// tools/make_contest_font.py writes (contest_font.h, ticker_font.h).
//
// Tokens are the run form of tools/contest_rle.py; yoff counts from the
// top of the capitals, lsb from the pen position.
#ifndef RUNGLYPH_H
#define RUNGLYPH_H

#include <stdint.h>

struct ContestGlyph {
  uint8_t c;           // the character, slashed O as 0x01
  uint8_t w, h;        // ink size; h 0 is a bare advance (space)
  int8_t yoff;         // ink top, counted from the top of the capitals
  int8_t lsb;          // ink left, counted from the pen position
  uint8_t adv;         // how far the pen moves
  const uint16_t *off; // h + 1 row offsets, or null
  const uint8_t *rle;  // the tokens, see tools/contest_rle.py
};

struct TickerGlyph {
  uint8_t c;
  uint8_t w, h;
  int8_t yoff;
  int8_t lsb;
  uint8_t adv;
  const uint16_t *off;
  const uint8_t *rle;
  // The ring for the transparent ticker: 3 wider on every side, 255
  // hard black, 128 half blended.
  const uint16_t *roff;
  const uint8_t *rrle;
};
#endif  // RUNGLYPH_H

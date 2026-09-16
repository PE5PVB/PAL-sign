#include "textoverlay.h"

#include <pico.h>   // __not_in_flash, for the ink cache below

#include "settings.h"

static int _font = 0;

// In RAM, same reason as the ticker's ink: read on every row, and
// rodata next to the flash tables costs a cache miss per row on a
// photograph card.
static YCbCr __not_in_flash("textink") inkId = {235, 128, 128};
static YCbCr __not_in_flash("textink") inkSub = {235, 128, 128};
static YCbCr __not_in_flash("textink") inkIns = {235, 128, 128};

YCbCr textInkId() { return inkId; }
YCbCr textInkSub() { return inkSub; }
YCbCr textInkInsert() { return inkIns; }

void textInkRefresh() {
  inkId = yccFromRgb(cfgCardColor(-1, CFG_COLOR_ID));
  inkSub = yccFromRgb(cfgCardColor(-1, CFG_COLOR_SUB));
  inkIns = yccFromRgb(cfgCardColor(-1, CFG_COLOR_INSERT));
}

// The edge cache further down is tied to a line and a font row, so it
// has to be dropped whenever a new line is prepared.
static void edgeForgetCache(void);

void textSetFont(int i) {
  if (i >= 0 && i < FONT_COUNT) _font = i;
}

int textGetFont(void) { return _font; }

const char *textFontName(int i) {
  return (i >= 0 && i < FONT_COUNT) ? FONTS[i].name : "?";
}

// PM5544: full cell width except punctuation. PM8546: ink width, except
// digits, which all get the width of the widest digit so a running
// clock does not jump sideways.
static int glyphCols(const Font *f, const Glyph *g, char c) {
  if (c >= '0' && c <= '9') return f->digits;
  return g->cols;
}

void textPrepare(TextOverlay *t, const char *s, int bx0, int bx1, int by0, int by1) {
  textPrepareScaled(t, s, bx0, bx1, by0, by1, 1);
}

void textPrepareScaled(TextOverlay *t, const char *s, int bx0, int bx1, int by0, int by1,
                          int scale) {
  const Font *f = &FONTS[_font];
  t->font = f;
  if (scale < 1) scale = 1;
  edgeForgetCache();

  int n = 0;
  int cols[TEXT_MAX_CHARS];
  for (const char *p = *s ? s : "", *e = p; *p && n < TEXT_MAX_CHARS; p++) {
    (void)e;
    uint8_t b = (uint8_t)*p;
    char c;
    // Slashed O in either encoding: UTF-8 0xC3 0x98/0xB8, Latin-1/CP1252
    // 0xD8/0xF8.
    if (b == 0xC3 && ((uint8_t)p[1] == 0x98 || (uint8_t)p[1] == 0xB8)) {
      c = GLYPH_OSLASH;
      p++;
    } else if (b == 0xD8 || b == 0xF8) {
      c = GLYPH_OSLASH;
    } else {
      c = (char)b;
    }

    const Glyph *g = 0;
    for (int i = 0; i < f->count; i++) {
      if (f->g[i].c == c) { g = &f->g[i]; break; }
    }
    if (!g && c >= 'a' && c <= 'z') {       // only the PM8546 has lower case
      char top = (char)(c - 'a' + 'A');
      for (int i = 0; i < f->count; i++) {
        if (f->g[i].c == top) { g = &f->g[i]; break; }
      }
    }
    if (!g) g = &f->g[0];                   // unknown character -> space
    cols[n] = glyphCols(f, g, c);
    t->glyph[n++] = g;
  }
  t->count = n;
  if (n == 0) {
    t->y0 = by0;
    t->step = 1;
    return;
  }

  // Is this font doubled in the table (e.g. the PM5544, 7x15 unfolded
  // to 14x30)? Checked, not assumed: if every odd row equals its
  // predecessor and every source column is two font columns wide, step
  // becomes 2, otherwise it stays 1. Matters for following edges below,
  // where a doubled row would otherwise look like it moves twice as
  // fast as it really does.
  t->step = 1;
  if ((f->rows & 1) == 0) {
    bool dubbel = true;
    for (int i = 0; i < n && dubbel; i++) {
      const uint32_t *r = t->glyph[i]->r;
      for (int k = 0; k < f->rows; k += 2) {
        if (r[k] != r[k + 1] || (((r[k] >> 1) ^ r[k]) & 0x55555555u)) { dubbel = false; break; }
      }
    }
    if (dubbel) t->step = 2;
  }

  // Centre on the real ink, not the cell height (which holds room for
  // descenders/accents most text never uses).
  int topRow = f->rows, bottomRow = -1;
  for (int i = 0; i < n; i++) {
    for (int r = 0; r < f->rows; r++) {
      if (t->glyph[i]->r[r]) {
        if (r < topRow) topRow = r;
        if (r > bottomRow) bottomRow = r;
      }
    }
  }
  if (bottomRow < 0) { topRow = 0; bottomRow = f->rows - 1; }

  // scale is a MAXIMUM: comes down until the text fits, in width and
  // height (shrinking the letter spacing alone is not enough once the
  // characters themselves are wider than the box).
  int kolTotaal = 0;
  for (int i = 0; i < n; i++) kolTotaal += cols[i];
  const int spaceW = (bx1 - bx0) - 6;  // 3 px margin either side
  const int spaceH = by1 - by0;
  while (scale > 1) {
    int wide = kolTotaal * f->sx * scale + (n - 1) * f->gap * scale;
    int high = (bottomRow - topRow + 1) * f->sy * scale;
    if (wide <= spaceW && high <= spaceH) break;
    scale--;
  }
  t->sx = (uint8_t)scale;
  t->sy = (uint8_t)scale;
  const int sx = f->sx * scale;
  const int sy = f->sy * scale;

  int inkH = (bottomRow - topRow + 1) * sy;
  t->y0 = by0 + ((by1 - by0) - inkH) / 2 - topRow * sy;

  int textW = 0;
  for (int i = 0; i < n; i++) textW += cols[i] * sx;

  // Letter spacing shrinks from the font's own gap only as far as it
  // must to fit, and scales with the enlargement so it does not vanish
  // at large scale.
  int gap = f->gap * scale;
  if (n > 1) {
    int fit = (spaceW - textW) / (n - 1);
    if (fit < gap) gap = fit;
    if (gap < 1) gap = 1;
  }
  int x = bx0 + ((bx1 - bx0) - (textW + (n - 1) * gap)) / 2;
  for (int i = 0; i < n; i++) {
    // A digit stands centred in the widest-digit width, or a narrow 1
    // sticks to the left and the clock looks crooked.
    t->x[i] = (int16_t)(x + ((cols[i] - t->glyph[i]->cols) / 2) * sx);
    x += cols[i] * sx + gap;
  }
}

void textFillBox(uint8_t *base, int x0, int x1, int y0, int y1, int y, YCbCr c) {
  if (y < y0 || y >= y1) return;
  fillSpanSolid(base, x0, x1, c);
}

// Following edges: the smooth edge for enlarged text. Enlarging a
// raster by a whole number turns every font pixel into a block, too big
// for ordinary antialiasing; the encoder smooths horizontally but not
// vertically, so the staircase is removed here.
//
// Per picture row, the edge position is interpolated between font rows
// (kept in sixteenths of a pixel) and the fractional coverage at a
// span's ends is written as a partial-pixel blend. An edge only counts
// as the same edge in the next row within EDGE_MAX_STEP columns
// (measured, not chosen), otherwise it stays put so real corners stay
// hard.
static const int EDGE_MAX_STEP = 2;

// The widest character in either font (the 'M') has 4 separate pieces
// of ink on its widest row; see tools/show_contest.py --measure.
static const int EDGE_MAX_RUNS = 4;

struct EdgePair {
  int16_t top, bottom;   // edge position, in sixteenths of a pixel, top/bottom of this font row
};

// One font row of the whole line; a font row covers sy picture rows
// (all sharing the same boundary values), so this is built once per
// font row, not per picture row.
struct EdgeChar {
  uint8_t n;
  EdgePair L[EDGE_MAX_RUNS], R[EDGE_MAX_RUNS];
};

static EdgeChar edgeCache[TEXT_MAX_CHARS];   // 408 bytes, the only extra RAM this costs
static const TextOverlay *edgeOwner = 0;
static int edgeFontRow = -1;

static void edgeForgetCache(void) {
  edgeOwner = 0;
  edgeFontRow = -1;
}

// Splits a font row into contiguous ink runs: l[k] the first column
// with ink, e[k] the first column without ink after it.
static int edgeRuns(uint32_t bits, uint8_t *l, uint8_t *e) {
  int n = 0, col = 0;
  while (bits && n < EDGE_MAX_RUNS) {
    int nul = __builtin_clz(bits);        // bits != 0, so 0..31
    bits <<= nul;
    col += nul;
    l[n] = (uint8_t)col;
    uint32_t inv = ~bits;
    int shift = inv ? __builtin_clz(inv) : 32;
    bits = (shift >= 32) ? 0u : (bits << shift);
    col += shift;
    e[n] = (uint8_t)col;
    n++;
  }
  return n;
}

// Left/right edge masks for a whole font row at once: column c sits in
// bit 31-c, so v >> 1 shifts column c-1 into column c's place.
static ALWAYS_INLINE uint32_t leftEdgeMask(uint32_t v) { return v & ~(v >> 1); }
static ALWAYS_INLINE uint32_t rightEdgeMask(uint32_t v) { return (v >> 1) & ~v; }

// Nearest matching edge in the neighbouring row's mask, searched both
// directions from e up to `max` columns (one direction alone breaks
// when a whole stroke shifts, e.g. the PM5544's zero). -1 if none
// within `max`, which is then a hard corner.
static int edgeNeighbour(uint32_t edges, int e, int max) {
  for (int d = 0; d <= max; d++) {
    for (int side = 0; side < 2; side++) {
      if (d == 0 && side) break;          // zero only needs doing once
      int c = side ? e - d : e + d;
      if (c >= 0 && c < 32 && ((edges >> (31 - c)) & 1u)) return c;
    }
  }
  return -1;
}

// The edge position on the boundary with row nb, halfway between this
// edge and its match there; stays put if no match.
static ALWAYS_INLINE int edgeBoundary(uint32_t edges, int e, int max) {
  int p = edgeNeighbour(edges, e, max);
  return (p < 0) ? 2 * e : e + p;
}

// A partly covered pixel, cov 0..16 sixteenths. Only luma blends: this
// only runs on the contest page, which is monochrome (chroma 128 both
// sides), and 4:2:2 chroma covers two pixels at once anyway.
static ALWAYS_INLINE void edgeBlendPixel(uint8_t *base, int x, uint8_t Y, int cov) {
  if (x < 0 || x >= SCREEN_W) return;
  int qb = (x >> 1) * 4 + ((x & 1) ? 3 : 1);
  int bg = base[qb];
  base[qb] = (uint8_t)((bg * (16 - cov) + (int)Y * cov + 8) >> 4);
}

// Boundary values of one font row for the whole line: the most
// expensive thing this file does, but only on the first picture row of
// a font row. Two shortcuts skip the search when the neighbour row is
// empty (top/bottom of a character, edges stay put) or equal to this
// one (zero shift, common with the PM5544's doubled rows and with
// straight-sided letters).
static void edgeFillCache(const TextOverlay *t, int gr, int sx, int step) {
  const Font *f = t->font;
  // Slack stays in font columns even with a doubled font: taking it in
  // source columns would treat a two-pixel-high serif as a sloping edge
  // and bevel it. Visible with tools/show_contest.py --hard/--png.
  (void)step;
  const int max = EDGE_MAX_STEP;
  for (int ci = 0; ci < t->count; ci++) {
    const uint32_t *r = t->glyph[ci]->r;
    uint32_t bits = r[gr];
    uint32_t above = (gr >= step) ? r[gr - step] : 0u;
    uint32_t below = (gr + step < f->rows) ? r[gr + step] : 0u;
    bool aboveFlat = (above == 0u) || (above == bits);
    bool belowFlat = (below == 0u) || (below == bits);
    uint32_t aboveL = 0, aboveR = 0, belowL = 0, belowR = 0;
    if (!aboveFlat) { aboveL = leftEdgeMask(above); aboveR = rightEdgeMask(above); }
    if (!belowFlat) { belowL = leftEdgeMask(below); belowR = rightEdgeMask(below); }
    uint8_t l[EDGE_MAX_RUNS], e[EDGE_MAX_RUNS];
    int n = edgeRuns(bits, l, e);
    EdgeChar *rt = &edgeCache[ci];
    rt->n = (uint8_t)n;
    int gx16 = t->x[ci] * 16;
    for (int k = 0; k < n; k++) {
      // Half a column -> picture pixel is times sx/2, then times 16 for
      // sixteenths.
      int lv = l[k] * sx * 16, ev = e[k] * sx * 16;
      rt->L[k].top = (int16_t)(aboveFlat ? gx16 + lv : gx16 + edgeBoundary(aboveL, l[k], max) * sx * 8);
      rt->L[k].bottom = (int16_t)(belowFlat ? gx16 + lv : gx16 + edgeBoundary(belowL, l[k], max) * sx * 8);
      rt->R[k].top = (int16_t)(aboveFlat ? gx16 + ev : gx16 + edgeBoundary(aboveR, e[k], max) * sx * 8);
      rt->R[k].bottom = (int16_t)(belowFlat ? gx16 + ev : gx16 + edgeBoundary(belowR, e[k], max) * sx * 8);
    }
  }
  edgeOwner = t;
  edgeFontRow = gr;
}

// An outline under the text, e.g. the TVE card's date/time sitting half
// on grey and half on a white grid line, where white on white would be
// invisible. Takes the font rows above/at/below y together per
// character and spreads the result one column each way; subtracting
// y's own row leaves the ring around it. Draw the text over the top of
// this. One font column wide (sx samples). True size only: enlarged
// text already follows its own edges, and a blocky outline there would
// undo that.
void textDrawRowOutline(uint8_t *base, const TextOverlay *t, int y, YCbCr c, int thickness) {
  if (t->count == 0) return;
  const Font *f = t->font;
  const int sx = f->sx * (t->sx ? t->sx : 1);
  const int sy = f->sy * (t->sy ? t->sy : 1);
  if (sy >= 2) return;
  if (thickness < 1) thickness = 1;

  if (y < t->y0 - thickness || y >= t->y0 + f->rows * sy + thickness) return;
  int gr = (y - t->y0) / sy;
  if (y < t->y0) gr = -1;  // division truncates towards zero, so force -1

  for (int ci = 0; ci < t->count; ci++) {
    const uint32_t *r = t->glyph[ci]->r;
    uint32_t band = 0;
    for (int k = gr - thickness; k <= gr + thickness; k++) {
      if (k >= 0 && k < f->rows) band |= r[k];
    }
    if (!band) continue;
    uint32_t own = (gr >= 0 && gr < f->rows) ? r[gr] : 0u;
    uint32_t bits = band;
    for (int s = 1; s <= thickness; s++) bits |= (band << s) | (band >> s);
    bits &= ~own;
    if (!bits) continue;

    int gx = t->x[ci];
    int col = 0;
    while (bits) {
      if (bits & 0x80000000u) {
        int start = col;
        while (bits & 0x80000000u) { bits <<= 1; col++; }
        fillSpanSolid(base, gx + start * sx, gx + col * sx, c);
      } else {
        bits <<= 1;
        col++;
      }
    }
  }
}

void textDrawRow(uint8_t *base, const TextOverlay *t, int y, YCbCr c) {
  if (t->count == 0) return;
  const Font *f = t->font;
  const int sx = f->sx * (t->sx ? t->sx : 1);
  const int sy = f->sy * (t->sy ? t->sy : 1);
  if (y < t->y0 || y >= t->y0 + f->rows * sy) return;
  int gr = (y - t->y0) / sy;

  // True size (sx=sy=1 for both fonts): the old, unchanged loop.
  if (sy < 2) {
    for (int ci = 0; ci < t->count; ci++) {
      uint32_t bits = t->glyph[ci]->r[gr];
      if (!bits) continue;
      int gx = t->x[ci];
      int col = 0;
      while (bits) {
        if (bits & 0x80000000u) {
          int start = col;
          while (bits & 0x80000000u) { bits <<= 1; col++; }
          fillSpanSolid(base, gx + start * sx, gx + col * sx, c);
        } else {
          bits <<= 1;
          col++;
        }
      }
    }
    return;
  }

  // A band is what lies between two edge control points: step font
  // rows (step x sy picture rows); with a doubled font that is two font
  // rows, since every odd one is a copy and not its own control point.
  const int step = t->step ? t->step : 1;
  const int bandH = step * sy;
  int band = (y - t->y0) / bandH;
  gr = band * step;
  if (gr > f->rows - step) gr = f->rows - step;

  if (edgeOwner != t || edgeFontRow != gr) edgeFillCache(t, gr, sx, step);

  // Position within the band, divided once per picture row instead of
  // per edge; rounds rather than truncates so the error stays under
  // half a sixteenth of a pixel.
  int dy = (y - t->y0) - band * bandH;
  int q = ((dy << 16) + bandH / 2) / bandH;

  for (int ci = 0; ci < t->count; ci++) {
    const EdgeChar *rt = &edgeCache[ci];
    for (int k = 0; k < rt->n; k++) {
      int XL = rt->L[k].top + (((rt->L[k].bottom - rt->L[k].top) * q + 32768) >> 16);
      int XR = rt->R[k].top + (((rt->R[k].bottom - rt->R[k].top) * q + 32768) >> 16);
      if (XR <= XL) continue;
      int a = XL >> 4, b = XR >> 4;
      if (a == b) {                       // narrower than one pixel
        edgeBlendPixel(base, a, c.Y, XR - XL);
        continue;
      }
      if (XL & 15) {
        edgeBlendPixel(base, a, c.Y, 16 - (XL & 15));
        a++;
      }
      fillSpanSolid(base, a, b, c);
      if (XR & 15) edgeBlendPixel(base, b, c.Y, XR & 15);
    }
  }
}

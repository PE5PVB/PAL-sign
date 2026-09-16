#include "ticker.h"

#include <string.h>

#include <Arduino.h>

#include "config.h"
#include "font_data.h"
#include "gfx.h"
#include "rletext.h"
#include "settings.h"
#include "ticker_font.h"

static_assert(TICKER_MAX_CHARS == CFG_TICKER_MAX, "the ticker text sizes have drifted apart");

// White from the factory, set from the PC palette; tickerBegin()
// converts sRGB with the same formula the photograph pipeline uses. The
// outline stays black whatever this is.
//
// In RAM, like the routines that use them: a photograph row's memcpy
// sweeps the XIP cache, so as plain rodata these would be re-fetched on
// every band row. Own section name, since a writable and a const symbol
// cannot share one named RAM section.
static YCbCr __not_in_flash("tickerink") tickerInk = {235, 128, 128};

// The outline in the transparent mode. Black, so it reads against the
// white ink whatever the card underneath is doing.
static const YCbCr __not_in_flash("ticker") TICKER_EDGE = {16, 128, 128};

// The ring around the letters, in samples: EDGE_HARD of solid black
// against the ink and EDGE_SOFT more blended halfway to it. Fixed at
// two and one, measured on the board as the best-reading width.
//
// The same on every card. Not offered at all on an uploaded photograph,
// where even the thinnest ring does not fit in the row budget.
static const int EDGE_HARD = 2;
static const int EDGE_SOFT = 1;
static const int EDGE_TOTAL = EDGE_HARD + EDGE_SOFT;
// The baked rings of the big face reach exactly this far; RING in
// tools/make_contest_font.py mirrors these widths and cannot see them.
static_assert(EDGE_TOTAL == 3, "regenerate contest_font.h with the new ring margin");

// Widens a font row by n columns to either side. Called with a constant,
// so the loop comes out unrolled.
static inline uint32_t widen(uint32_t b, int n) {
  uint32_t o = b;
  for (int k = 1; k <= n; k++) o |= (b << k) | (b >> k);
  return o;
}

// 0x10801080 little endian is bytes 80,10,80,10: chroma neutral, luma
// at black level.
static const uint32_t TICKER_BAR_WORD = 0x10801080u;

// Air above and below the letters. Even, so the height of the band stays
// even whatever the font does.
static const int TICKER_PAD = 4;

// Blank between the end of the message and the start of the next turn,
// so a short message does not run into itself.
static const int TICKER_LOOP_GAP = 96;

// The speed ladder, indexed by the size of cfgTickerSpeed(); the sign
// of that value is the direction and 0 is a band that stands still.
// Every step is an even count of samples on purpose (see the header);
// the slow rungs step 2 samples every DIV pictures, the fast ones more
// samples every picture. Rung 4 is the fixed 2 a picture the band
// always had.
static const uint8_t SPEED_DIV[9] = {1, 4, 3, 2, 1, 1, 1, 1, 1};
static const uint8_t SPEED_STEP[9] = {2, 2, 2, 2, 2, 4, 6, 8, 10};
static int speedPhase = 0;   // pictures waited on the slow rungs

// settings.cpp holds the truth; everything below is a cache that
// tickerBegin() rebuilds. The glyph bitmaps are copied into RAM and the
// layout points at the copy, since font_data.h lives in flash and a
// photograph card's row copy sweeps the XIP cache. Only the distinct
// characters of the message are copied; running out of room just leaves
// the rest pointing at flash, slower but never wrong.
static const int TICKER_GLYPH_MAX = 64;
static const int TICKER_GLYPH_WORDS = 1664;  // the whole alphabet of any one font fits
static uint32_t glyphRam[TICKER_GLYPH_WORDS];
static const Glyph *ramKey[TICKER_GLYPH_MAX];
static const uint32_t *ramRows[TICKER_GLYPH_MAX];
static int ramN = 0, ramUsed = 0;

// The two rings are worked out once, here, and not per row: the answer
// is the same for a given glyph at a given font row every time.
//
// Two words per glyph per row, over the rows of the glyph plus the ring
// on either side. Does not fit and the pointer stays null, and that
// glyph is worked out per row as before: slower, never wrong.
static const int TICKER_RING_WORDS = 2048;
static uint32_t ringRam[TICKER_RING_WORDS];
static int ringUsed = 0;

// Inter and Doto draw as run glyphs (ticker_font.h, built by
// tools/make_ticker_font.py), which no 32 bit glyph row could hold, a
// capital there being about 45 columns wide. Each comes in FIVE FIXED
// SIZES (35/45/60/70/90 row capitals), every one drawn 1:1, no runtime
// scaling. Ten entries, BIG_FACES[face * TICKER_SIZE_COUNT + step];
// bigFace is which one (or none) is up. The transparent mode rides its
// own rings either way: each generator bakes them per glyph at the same
// widths the raster path uses, 255 for the hard black and 128 for the
// soft edge, three columns and rows around the ink; the static assert
// below is what those three lean on, for every size of both faces alike.
//
// top/bottom are the outlier-inclusive extent (parentheses and the like
// reach further than an ordinary capital), needed so the band is tall
// enough that nothing clips. cap is the plain capital's own height:
// every glyph's yoff already counts from that reference, so no separate
// capTop is needed the way the raster faces below have. pad is the air
// the ticker puts above and below cap when laying out the band.
struct BigFace {
  const TickerGlyph *g;
  int n;
  int top, bottom, cap, pad;
};

static const int TICKER_PAD_SQUARE = 8;  // double TICKER_PAD, Doto reads better with more air

// One BigFace literal from a ticker_font.h symbol prefix (g##_TOP etc.
// are real symbols make_ticker_font.py wrote out): five of these per
// face, once for each fixed size.
#define BIG_FACE_ENTRY(g, facePad) \
  { g, (int)(sizeof(g) / sizeof(g[0])), g##_TOP, g##_BOTTOM, g##_CAP, facePad }

static const int TICKER_SIZE_COUNT = 5;   // extra small .. extra large
static const BigFace BIG_FACES[] = {
    BIG_FACE_ENTRY(TICKER_FONT_XSMALL, TICKER_PAD),
    BIG_FACE_ENTRY(TICKER_FONT_SMALL, TICKER_PAD),
    BIG_FACE_ENTRY(TICKER_FONT, TICKER_PAD),
    BIG_FACE_ENTRY(TICKER_FONT_LARGE, TICKER_PAD),
    BIG_FACE_ENTRY(TICKER_FONT_XLARGE, TICKER_PAD),
    BIG_FACE_ENTRY(TICKER_FONT_SQUARE_XSMALL, TICKER_PAD_SQUARE),
    BIG_FACE_ENTRY(TICKER_FONT_SQUARE_SMALL, TICKER_PAD_SQUARE),
    BIG_FACE_ENTRY(TICKER_FONT_SQUARE, TICKER_PAD_SQUARE),
    BIG_FACE_ENTRY(TICKER_FONT_SQUARE_LARGE, TICKER_PAD_SQUARE),
    BIG_FACE_ENTRY(TICKER_FONT_SQUARE_XLARGE, TICKER_PAD_SQUARE),
};
#undef BIG_FACE_ENTRY
static const int TICKER_FACE_BIG_INTER = 2;    // FONTS[2], named Inter -> BIG_FACES[0..4]
static const int TICKER_FACE_BIG_SQUARE = 3;   // FONTS[3], named Doto -> BIG_FACES[5..9]

// -1: the ordinary raster path is up. Otherwise an index into
// BIG_FACES.
static int bigFace = -1;
static uint8_t bigLut[256];

// step: 0..4, cfgCardTickerSize() clamped; see settings.h.
static int bigFaceFor(int fontIndex, int step) {
  if (step < 0) step = 0;
  if (step >= TICKER_SIZE_COUNT) step = TICKER_SIZE_COUNT - 1;
  if (fontIndex == TICKER_FACE_BIG_INTER) return 0 * TICKER_SIZE_COUNT + step;
  if (fontIndex == TICKER_FACE_BIG_SQUARE) return 1 * TICKER_SIZE_COUNT + step;
  return -1;
}

// The token streams of the message, copied into RAM as far as they fit.
// The two raster pools above lie idle while the big face is up, so they
// are reused byte for byte; the distinct characters of the message are
// copied most frequent first, so one copy serves every occurrence.
// Running out of room just leaves the rest pointing at flash.
//
// The geometry rides along so a band row never touches the flash
// struct at all.
struct BigGlyph {
  const TickerGlyph *key;  // the flash glyph this stands for
  const uint8_t *rle;      // ink tokens, RAM or flash
  const uint16_t *off;
  const uint8_t *rrle;     // ring tokens, RAM or flash
  const uint16_t *roff;
  uint8_t w, h;
  int8_t yoff, lsb;
};
static BigGlyph bigCache[TICKER_GLYPH_MAX];
static int bigN = 0;
static int bigUsedA = 0, bigUsedB = 0;   // bytes taken of glyphRam resp. ringRam

static const BigGlyph *bigG[TICKER_MAX_CHARS];

static const uint32_t *gl[TICKER_MAX_CHARS];
static const uint32_t *glRing[TICKER_MAX_CHARS];
static int16_t gx[TICKER_MAX_CHARS];
static uint8_t gw[TICKER_MAX_CHARS];
static int gn = 0;
static int turnW = 0;
static int offset = 0;
static int bandY = 0;
static int bandH = 0;
// The actual top/bottom margin around the capital, equal both sides.
// Can exceed the face's own requested pad, when an outlier forces it
// wider.
static int bandMargin = 0;
static const Font *face = &FONTS[0];

// What the row routines lean on, copied into RAM: they run for every
// row of the band, while cfgTickerMode() and the Font itself live in
// flash. Same reason the routines carry __not_in_flash_func.
static int runMode = TICKER_OFF;
static bool paused = false;
static bool allowTransparent = true;
static int faceRows = 0;
static uint8_t faceSx = 1, faceSy = 1;
static int faceCapTop = 0;   // see Font's own capTop/capRows in font_data.h

// What is on the screen at this offset, worked out once per picture so
// that the row loop has nothing to decide. A glyph can appear twice when
// the seam of the loop is in view, hence the room for one more turn.
struct Visible {
  const uint32_t *r;     // the ink, one word a font row
  const uint32_t *ring;  // hard and soft in pairs, or null
  const BigGlyph *big;   // the run glyph when the big face is up
  int16_t x;
};
static Visible vis[TICKER_MAX_CHARS * 2];
static int visN = 0;

static uint32_t statRows = 0, statSumUs = 0, statWorstUs = 0, statGlyphs = 0;

static inline void statAdd(uint32_t us) {
  statRows++;
  statSumUs += us;
  statGlyphs += (uint32_t)visN;
  if (us > statWorstUs) statWorstUs = us;
}

void tickerTakeStats(uint32_t *rows, uint32_t *sumUs, uint32_t *worstUs, uint32_t *glyphs) {
  *rows = statRows;
  *sumUs = statSumUs;
  *worstUs = statWorstUs;
  *glyphs = statGlyphs;
  statRows = statSumUs = statWorstUs = statGlyphs = 0;
}

// The same rule as textPrepare(): the digits all get the width of the
// widest one, so a message with figures in it does not jitter as it
// goes past.
static int glyphCols(const Font *f, const Glyph *g, char c) {
  if (c >= '0' && c <= '9') return f->digits;
  return g->cols;
}

// Looks a character up the way textPrepare() does: the slashed O in
// either encoding, lower case folded up where the font has no lower
// case, anything unknown as a space.
static const Glyph *lookUp(const Font *f, char c) {
  for (int i = 0; i < f->count; i++) {
    if (f->g[i].c == c) return &f->g[i];
  }
  if (c >= 'a' && c <= 'z') {
    const char up = (char)(c - 'a' + 'A');
    for (int i = 0; i < f->count; i++) {
      if (f->g[i].c == up) return &f->g[i];
    }
  }
  return &f->g[0];
}

// Hands back the rows of this glyph in RAM, copying it in the first time
// it is asked for. Only called while laying out, so the linear search
// costs nothing that matters.
static const uint32_t *toRam(const Glyph *g, int rows) {
  for (int i = 0; i < ramN; i++) {
    if (ramKey[i] == g) return ramRows[i];
  }
  if (ramN >= TICKER_GLYPH_MAX || ramUsed + rows > TICKER_GLYPH_WORDS) return g->r;
  uint32_t *dst = &glyphRam[ramUsed];
  memcpy(dst, g->r, (size_t)rows * sizeof(uint32_t));
  ramKey[ramN] = g;
  ramRows[ramN] = dst;
  ramN++;
  ramUsed += rows;
  return dst;
}

// Works the two rings out for every row this glyph can be drawn on, and
// hands back where they went, or null when there is no room left.
//
// The rings are kept shifted right by EDGE_TOTAL columns, so widening
// to the left cannot push ink out of the word; they are drawn that many
// columns further left again.
static const uint32_t *buildRing(const uint32_t *ink, int rows) {
  const int n = rows + 2 * EDGE_TOTAL;
  if (ringUsed + 2 * n > TICKER_RING_WORDS) return 0;
  uint32_t *dst = &ringRam[ringUsed];
  ringUsed += 2 * n;
  for (int i = 0; i < n; i++) {
    const int gr = i - EDGE_TOTAL;
    uint32_t near = 0, far = 0;
    for (int k = gr - EDGE_TOTAL; k <= gr + EDGE_TOTAL; k++) {
      if (k < 0 || k >= rows) continue;
      const uint32_t v = ink[k] >> EDGE_TOTAL;
      if (k >= gr - EDGE_HARD && k <= gr + EDGE_HARD) near |= v;
      far |= v;
    }
    const uint32_t own = (gr >= 0 && gr < rows) ? (ink[gr] >> EDGE_TOTAL) : 0u;
    const uint32_t hard = widen(near, EDGE_HARD) & ~own;
    dst[2 * i] = hard;
    dst[2 * i + 1] = widen(far, EDGE_TOTAL) & ~hard & ~own;
  }
  return dst;
}

int tickerHeight() { return bandH; }

// One block out of the two idle pools, even so a uint16 offset table
// can sit at its start; null when neither has the room.
static uint8_t *bigAlloc(int len) {
  bigUsedA = (bigUsedA + 1) & ~1;
  if (bigUsedA + len <= (int)sizeof(glyphRam)) {
    uint8_t *p = (uint8_t *)glyphRam + bigUsedA;
    bigUsedA += len;
    return p;
  }
  bigUsedB = (bigUsedB + 1) & ~1;
  if (bigUsedB + len <= (int)sizeof(ringRam)) {
    uint8_t *p = (uint8_t *)ringRam + bigUsedB;
    bigUsedB += len;
    return p;
  }
  return 0;
}

// The offsets and the tokens of one stream in one block: the table
// first, at the even start, the tokens right behind it.
static bool bigCopyStream(const uint16_t *off, const uint8_t *rle, int rows,
                          const uint16_t **offOut, const uint8_t **rleOut) {
  const int offBytes = (rows + 1) * (int)sizeof(uint16_t);
  const int rleBytes = off[rows];
  uint8_t *p = bigAlloc(offBytes + rleBytes);
  if (!p) return false;
  memcpy(p, off, (size_t)offBytes);
  memcpy(p + offBytes, rle, (size_t)rleBytes);
  *offOut = (const uint16_t *)p;
  *rleOut = p + offBytes;
  return true;
}

// Hands back the RAM copy of this glyph, making it the first time it is
// asked for; ink and ring fall back on flash separately when the pools
// run dry. Only called while laying out, like toRam() above.
static const BigGlyph *toRamBig(const TickerGlyph *g) {
  for (int i = 0; i < bigN; i++) {
    if (bigCache[i].key == g) return &bigCache[i];
  }
  if (bigN >= TICKER_GLYPH_MAX) return 0;
  BigGlyph *d = &bigCache[bigN++];
  d->key = g;
  d->w = g->w;
  d->h = g->h;
  d->yoff = g->yoff;
  d->lsb = g->lsb;
  d->rle = g->rle;
  d->off = g->off;
  d->rrle = g->rrle;
  d->roff = g->roff;
  if (g->h) {
    bigCopyStream(g->off, g->rle, g->h, &d->off, &d->rle);
    bigCopyStream(g->roff, g->rrle, g->h + 2 * EDGE_TOTAL, &d->roff, &d->rrle);
  }
  return d;
}

// One character of the message to its big glyph, out of the currently
// selected big face (BIG_FACES[bigFace], set in tickerBegin()): the
// slashed O whatever the file encoding (UTF-8 is 0xC3 0x98 resp. 0xC3
// 0xB8, Latin-1 and CP1252 are 0xD8 resp. 0xF8), lower case folded up,
// unknown to the space. Eats the second byte of the UTF-8 pair, hence
// the double pointer; the caller's own p++ takes the first.
static const TickerGlyph *tickerGlyphFor(const char **pp) {
  const char *p = *pp;
  const uint8_t b = (uint8_t)*p;
  char c;
  if (b == 0xC3 && ((uint8_t)p[1] == 0x98 || (uint8_t)p[1] == 0xB8)) {
    c = GLYPH_OSLASH;
    *pp = p + 1;
  } else if (b == 0xD8 || b == 0xF8) {
    c = GLYPH_OSLASH;
  } else if (b >= 'a' && b <= 'z') {
    c = (char)(b - 'a' + 'A');
  } else {
    c = (char)b;
  }
  const BigFace &bf = BIG_FACES[bigFace];
  for (int i = 0; i < bf.n; i++) {
    if (bf.g[i].c == (uint8_t)c) return &bf.g[i];
  }
  return &bf.g[0];   // unknown -> space
}

static void rebuildVisible() {
  visN = 0;
  if (gn == 0) return;
  // Two turns are enough because turnW is never less than a screen wide.
  for (int turn = 0; turn < 2; turn++) {
    const int base = turn * turnW - offset;
    for (int i = 0; i < gn; i++) {
      const int x = base + gx[i];
      if (x >= SCREEN_W) break;  // gx rises, so the rest is off to the right
      if (x + gw[i] <= 0) continue;
      if (visN >= (int)(sizeof(vis) / sizeof(vis[0]))) return;
      vis[visN].r = bigFace >= 0 ? 0 : gl[i];
      vis[visN].ring = bigFace >= 0 ? 0 : glRing[i];
      vis[visN].big = bigFace >= 0 ? bigG[i] : 0;
      vis[visN].x = (int16_t)x;
      visN++;
    }
  }
}

void tickerBegin() {
  int fi = cfgTickerFont();
  if (fi < 0 || fi >= FONT_COUNT) fi = 0;
  bigFace = bigFaceFor(fi, cfgTickerSize());
  face = &FONTS[fi];
  const Font *f = face;
  faceRows = f->rows;
  faceSx = f->sx;
  faceSy = f->sy;
  faceCapTop = f->capTop;
  runMode = cfgCardTicker();
  if (runMode == TICKER_TRANSPARENT && !allowTransparent) runMode = TICKER_SOLID;

  tickerInk = yccFromRgb(cfgCardColor(-1, CFG_COLOR_TICKER));

  const int sx = faceSx;

  // The band's top/bottom margin, kept equal on both sides: the margin
  // grows on both sides together by whichever is largest of pad, how
  // far the highest outlier reaches above the capital, or how far the
  // deepest one reaches below it. margin = max(pad, above, below), band
  // height 2 x margin + cap. A face with no outlier of its own is
  // unaffected.
  const int pad = bigFace >= 0 ? BIG_FACES[bigFace].pad : TICKER_PAD;
  int cap, above, below;
  if (bigFace >= 0) {
    // Run glyph rows are already picture rows, no sy of their own to
    // scale by. An ordinary capital's own yoff is 0, so top itself
    // (always <= 0) is exactly how far the highest outlier reaches
    // above it.
    const BigFace &bf = BIG_FACES[bigFace];
    cap = bf.cap;
    above = -bf.top;
    below = bf.bottom - bf.cap;
  } else {
    // capTop/capRows/rows are in the SAME raw font rows (see
    // make_fonts.py's cap_reference()), so they need the same sy to
    // become picture rows.
    cap = f->capRows * f->sy;
    above = f->capTop * f->sy;
    below = (f->rows - f->capTop) * f->sy - cap;
  }
  const int margin = pad > above ? (pad > below ? pad : below) : (above > below ? above : below);
  bandMargin = margin;
  // Rounded up, not down: rounding down would eat back into the margin
  // this calculation exists to keep symmetric.
  bandH = (2 * margin + cap + 1) & ~1;

  // Keep the band on the screen and its top row even: an odd row would
  // put the two edges of the band in different fields, flickering at
  // 25 Hz.
  bandY = cfgTickerY();
  if (bandY > SCREEN_H - bandH) bandY = SCREEN_H - bandH;
  if (bandY < 0) bandY = 0;
  bandY &= ~1;

  gn = 0;
  ramN = 0;
  ramUsed = 0;
  ringUsed = 0;
  // Height and Y are kept for tickerSetY() to read back even with the
  // band off.
  if (runMode == TICKER_OFF) return;
  if (bigFace >= 0) {
    // Coverage to luma, black level up to the ink; against the ink's
    // own Y, so a coloured letter fades to its colour and not to white.
    for (int i = 0; i < 256; i++) {
      bigLut[i] = (uint8_t)(16 + ((tickerInk.Y - 16) * i) / 255);
    }
    bigN = 0;
    bigUsedA = 0;
    bigUsedB = 0;

    // The pools filled most frequent character first: the room holds
    // about eight glyphs and a message repeats its letters, so the
    // counted order puts the copies where the most rows are drawn from.
    {
      const TickerGlyph *dg[TICKER_GLYPH_MAX];
      uint16_t dc[TICKER_GLYPH_MAX];
      int dn = 0, seen = 0;
      for (const char *p = cfgTickerText(); *p && seen < TICKER_MAX_CHARS; p++, seen++) {
        const TickerGlyph *g = tickerGlyphFor(&p);
        int k = 0;
        while (k < dn && dg[k] != g) k++;
        if (k == dn) {
          if (dn >= TICKER_GLYPH_MAX) continue;
          dg[dn] = g;
          dc[dn++] = 0;
        }
        dc[k]++;
      }
      for (int i = 0; i < dn; i++) {   // selection sort; dn is at most the font
        int best = i;
        for (int k = i + 1; k < dn; k++) {
          if (dc[k] > dc[best]) best = k;
        }
        const TickerGlyph *tg = dg[i];
        uint16_t tc = dc[i];
        dg[i] = dg[best];
        dc[i] = dc[best];
        dg[best] = tg;
        dc[best] = tc;
        toRamBig(dg[i]);
      }
    }

    int bx = 0;
    for (const char *p = cfgTickerText(); *p && gn < TICKER_MAX_CHARS; p++) {
      const TickerGlyph *g = tickerGlyphFor(&p);
      bigG[gn] = toRamBig(g);   // found in the cache, filled just above
      gx[gn] = (int16_t)bx;
      // The culling width: the ink can start left of the pen (lsb) and
      // reach past the advance, so the wider of the two counts.
      const int inkW = g->lsb + g->w;
      gw[gn] = (uint8_t)(inkW > g->adv ? inkW : g->adv);
      gn++;
      bx += g->adv;   // the face's own spacing, no gap bolted on
    }
    turnW = bx + TICKER_LOOP_GAP;
    if (turnW < SCREEN_W + TICKER_LOOP_GAP) turnW = SCREEN_W + TICKER_LOOP_GAP;
    turnW &= ~1;
    if (offset >= turnW) offset = 0;
    rebuildVisible();
    return;
  }
  int x = 0;
  for (const char *p = cfgTickerText(); *p && gn < TICKER_MAX_CHARS; p++) {
    const uint8_t b = (uint8_t)*p;
    char c;
    // The slashed O whatever the file encoding: UTF-8 is 0xC3 0x98 (resp.
    // 0xC3 0xB8), Latin-1 and CP1252 are 0xD8 (resp. 0xF8).
    if (b == 0xC3 && ((uint8_t)p[1] == 0x98 || (uint8_t)p[1] == 0xB8)) {
      c = GLYPH_OSLASH;
      p++;
    } else if (b == 0xD8 || b == 0xF8) {
      c = GLYPH_OSLASH;
    } else {
      c = (char)b;
    }
    const Glyph *g = lookUp(f, c);
    const int w = glyphCols(f, g, c) * sx;
    gl[gn] = toRam(g, faceRows);
    // The same glyph twice in the message shares one set of rings.
    glRing[gn] = 0;
    for (int k = 0; k < gn; k++) {
      if (gl[k] == gl[gn]) { glRing[gn] = glRing[k]; break; }
    }
    if (!glRing[gn]) glRing[gn] = buildRing(gl[gn], faceRows);
    gx[gn] = (int16_t)x;
    gw[gn] = (uint8_t)w;
    gn++;
    x += w + f->gap;
  }

  // A turn is the message plus a blank, and never narrower than the
  // screen plus that blank, or a short message would show more than
  // twice at once and rebuildVisible() would miss the third.
  turnW = x + TICKER_LOOP_GAP;
  if (turnW < SCREEN_W + TICKER_LOOP_GAP) turnW = SCREEN_W + TICKER_LOOP_GAP;
  turnW &= ~1;  // keeps the seam on an even sample, like the step itself

  if (offset >= turnW) offset = 0;
  rebuildVisible();
}

void tickerFrame() {
  // Once a picture, so the row routines never have to reach into flash
  // for it. Also where a change of card is picked up: the mode is kept
  // per card, and selectPattern() has told settings.cpp which card is
  // active by the time this runs.
  runMode = cfgCardTicker();
  if (runMode == TICKER_TRANSPARENT && !allowTransparent) runMode = TICKER_SOLID;
  if (runMode == TICKER_OFF || gn == 0) return;
  int v = cfgTickerSpeed();
  if (v < -8) v = -8;
  if (v > 8) v = 8;
  // Zero is not on offer (the setter snaps it away), so this only
  // guards a record written by hand; the band then stands.
  if (v == 0) return;
  const int lvl = v < 0 ? -v : v;
  if (SPEED_DIV[lvl] > 1) {
    // A slow rung: the band stands still this picture, same as above.
    if (++speedPhase < SPEED_DIV[lvl]) return;
    speedPhase = 0;
  }
  // Negative walks left (offset up, the way the band always walked),
  // positive walks right; both wraps land on an even offset because
  // every step and turnW are even.
  if (v < 0) {
    offset += SPEED_STEP[lvl];
    if (offset >= turnW) offset -= turnW;
  } else {
    offset -= SPEED_STEP[lvl];
    if (offset < 0) offset += turnW;
  }
  rebuildVisible();
}

// Jumps from run to run instead of walking bit by bit: counting leading
// zeros and then leading ones costs two turns per piece of ink, instead
// of one loop turn per bit whatever is in the word.
//
// fillSpanSolid() clips both ends, so a glyph half off the screen needs
// nothing of its own.
static void __not_in_flash_func(drawBits)(uint8_t *base, uint32_t bits, int x0, int sx,
                                         YCbCr c) {
  int col = 0;
  while (bits) {
    // bits is not zero here, so __builtin_clz is defined.
    const int skip = __builtin_clz(bits);
    bits <<= skip;
    col += skip;
    // The top bit is set now, so ~bits has it clear and clz is defined,
    // except when the whole word is ink; that one case is taken by hand.
    const int run = (bits == 0xFFFFFFFFu) ? 32 : __builtin_clz(~bits);
    fillSpanSolid(base, x0 + col * sx, x0 + (col + run) * sx, c);
    col += run;
    if (run >= 32) break;  // shifting a 32 bit word by 32 is undefined
    bits <<= run;
  }
}

// Pulls the luma of [x0,x1) halfway towards y and leaves the chroma
// alone: blending the chroma too would drag the colour underneath
// towards grey, a pale fringe on a saturated card.
//
// Clips like fillSpanSolid(), so a glyph half off the screen is safe.
static void __not_in_flash_func(blendSpanLuma)(uint8_t *base, int x0, int x1, uint8_t y) {
  if (x0 < 0) x0 = 0;
  if (x1 > SCREEN_W) x1 = SCREEN_W;
  for (int x = x0; x < x1; x++) {
    uint8_t *p = &base[(x >> 1) * 4 + ((x & 1) ? 3 : 1)];
    *p = (uint8_t)((*p + y) >> 1);
  }
}

// The same span walk as drawBits(), but blending instead of painting.
static void __not_in_flash_func(blendBits)(uint8_t *base, uint32_t bits, int x0, int sx,
                                          uint8_t y) {
  int col = 0;
  while (bits) {
    const int skip = __builtin_clz(bits);
    bits <<= skip;
    col += skip;
    const int run = (bits == 0xFFFFFFFFu) ? 32 : __builtin_clz(~bits);
    blendSpanLuma(base, x0 + col * sx, x0 + (col + run) * sx, y);
    col += run;
    if (run >= 32) break;
    bits <<= run;
  }
}

// The font row that belongs to picture row y, or -1 when y is padding.
// Anchored on the capital, not on font row 0: font row faceCapTop,
// where an ordinary capital's own ink starts, is put exactly bandMargin
// below the band top, because font row 0 itself can sit above the
// capital if some other character in the set reaches higher. faceCapTop
// is in font rows, like gr itself, so it is added AFTER dividing the
// picture-row distance by faceSy, not before.
static int fontRowOf(int y) {
  const int gr = faceCapTop + (y - bandY - bandMargin) / faceSy;
  return (gr < 0 || gr >= faceRows) ? -1 : gr;
}

bool __not_in_flash_func(tickerSolidRow)(int y) {
  if (runMode != TICKER_SOLID || gn == 0 || paused) return false;
  return y >= bandY && y < bandY + bandH;
}

void __not_in_flash_func(tickerRenderRow)(uint8_t *base, int y) {
  const uint32_t t0 = time_us_32();
  uint32_t *w = (uint32_t *)base;
  for (int i = 0; i < SCREEN_W / 2; i++) *w++ = TICKER_BAR_WORD;

  if (bigFace >= 0) {
    // The run glyphs, each on its own rows: yoff counts from the top of
    // the capitals, which sit bandMargin below the band top -- an
    // ordinary capital's own yoff is 0, so this puts it exactly
    // bandMargin rows down.
    const int capRow = bandY + bandMargin;
    for (int i = 0; i < visN; i++) {
      const BigGlyph *g = vis[i].big;
      if (!g || !g->h) continue;
      const int r = y - (capRow + g->yoff);
      if (r < 0 || r >= g->h) continue;
      rleBlendRow(base, vis[i].x + g->lsb, g->rle + g->off[r], g->rle + g->off[r + 1],
                  bigLut, tickerInk.Y, tickerInk.Cb, tickerInk.Cr, SCREEN_W);
    }
    statAdd(time_us_32() - t0);
    return;
  }
  const int gr = fontRowOf(y);
  if (gr >= 0) {  // below zero it is a padding row: the bar and no more
    const int sx = faceSx;
    for (int i = 0; i < visN; i++) {
      const uint32_t bits = vis[i].r[gr];
      if (bits) drawBits(base, bits, vis[i].x, sx, tickerInk);
    }
  }
  statAdd(time_us_32() - t0);
}

void __not_in_flash_func(tickerOverlayRow)(uint8_t *base, int y) {
  if (runMode != TICKER_TRANSPARENT || gn == 0 || paused) return;
  if (y < bandY || y >= bandY + bandH) return;
  const uint32_t t0 = time_us_32();

  if (bigFace >= 0) {
    // All the rings first, then the ink: the ring of the next letter
    // would otherwise eat into the ink of the one before it. The ring
    // bitmaps are three wider on every side than the ink and their own
    // rows start EDGE_TOTAL above it.
    const int capRow = bandY + bandMargin;
    for (int i = 0; i < visN; i++) {
      const BigGlyph *g = vis[i].big;
      if (!g || !g->h) continue;
      const int rr = y - (capRow + g->yoff - EDGE_TOTAL);
      if (rr < 0 || rr >= g->h + 2 * EDGE_TOTAL) continue;
      rleDarkenRow(base, vis[i].x + g->lsb - EDGE_TOTAL, g->rrle + g->roff[rr],
                   g->rrle + g->roff[rr + 1], SCREEN_W);
    }
    for (int i = 0; i < visN; i++) {
      const BigGlyph *g = vis[i].big;
      if (!g || !g->h) continue;
      const int r = y - (capRow + g->yoff);
      if (r < 0 || r >= g->h) continue;
      rleBlendRow(base, vis[i].x + g->lsb, g->rle + g->off[r], g->rle + g->off[r + 1],
                  bigLut, tickerInk.Y, tickerInk.Cb, tickerInk.Cr, SCREEN_W);
    }
    statAdd(time_us_32() - t0);
    return;
  }

  // Three rows wider on either side than the ink, as far as the outer
  // ring reaches. The range is settled first and the font row worked
  // out afterwards: division truncates towards zero, so every padding
  // row above the ink would otherwise come out as row 0 and get the
  // ring drawn on it too.
  const int rows = faceRows;
  const int top = bandY + bandMargin - faceCapTop * faceSy;   // anchored like fontRowOf()
  if (y < top - EDGE_TOTAL || y >= top + rows * faceSy + EDGE_TOTAL) {
    statAdd(time_us_32() - t0);
    return;
  }
  int gr = (y - top) / faceSy;
  if (y < top) gr = -1 - ((top - 1 - y) / faceSy);

  const int sx = faceSx;

  // The glyph shifted right by EDGE_TOTAL columns, drawn that many
  // columns further left to make up for it: column 0 is bit 31, so
  // widening to the left would push ink out of the word, and these
  // fonts have no margin of their own. The widest glyph is 18 columns,
  // so three of headroom leaves nothing running off the other end.
  const int x0 = 0 - EDGE_TOTAL * sx;

  // All the rings of all the glyphs first, then the ink, same reason as
  // the big-face path above. Rings come from the table buildRing()
  // filled in while laying out; only a glyph that found no room there
  // is worked out here.
  const int ri = (gr + EDGE_TOTAL) * 2;
  for (int i = 0; i < visN; i++) {
    uint32_t hard, soft;
    const uint32_t *ring = vis[i].ring;
    if (ring) {
      hard = ring[ri];
      soft = ring[ri + 1];
      if (!(hard | soft)) continue;
    } else {
      const uint32_t *r = vis[i].r;
      uint32_t near = 0, far = 0;
      for (int k = gr - EDGE_TOTAL; k <= gr + EDGE_TOTAL; k++) {
        if (k < 0 || k >= rows) continue;
        const uint32_t v = r[k] >> EDGE_TOTAL;
        if (k >= gr - EDGE_HARD && k <= gr + EDGE_HARD) near |= v;
        far |= v;
      }
      if (!far) continue;
      const uint32_t own = (gr >= 0 && gr < rows) ? (r[gr] >> EDGE_TOTAL) : 0u;
      hard = widen(near, EDGE_HARD) & ~own;
      soft = widen(far, EDGE_TOTAL) & ~hard & ~own;
    }
    const int x = vis[i].x + x0;
    // The soft one first: it is the wider of the two, so painting the
    // hard ring over it needs no mask beyond the one above.
    if (soft) blendBits(base, soft, x, sx, TICKER_EDGE.Y);
    if (hard) drawBits(base, hard, x, sx, TICKER_EDGE);
  }
  if (gr >= 0 && gr < rows) {
    for (int i = 0; i < visN; i++) {
      const uint32_t bits = vis[i].r[gr];
      if (bits) drawBits(base, bits, vis[i].x, sx, tickerInk);
    }
  }
  statAdd(time_us_32() - t0);
}

void __not_in_flash_func(tickerPause)(bool v) { paused = v; }

void tickerAllowTransparent(bool v) {
  allowTransparent = v;
  if (!v && runMode == TICKER_TRANSPARENT) runMode = TICKER_SOLID;
}

int tickerMode() { return runMode; }

void tickerSetMode(int m) {
  if (m < 0 || m >= TICKER_MODE_COUNT) return;
  cfgSetCardTicker(m);
  runMode = m;
  // The band itself too: if the ticker was off at the last
  // tickerBegin() not a single glyph is laid out (gn == 0) and the
  // mode change alone showed nothing.
  tickerBegin();
}

const char *tickerModeName() {
  switch (runMode) {
    case TICKER_SOLID: return "solid";
    case TICKER_TRANSPARENT: return "transparent";
    default: return "off";
  }
}

int tickerY() { return bandY; }

void tickerSetY(int y) {
  cfgSetTickerY(y);
  tickerBegin();  // clamps and stores the result back
  cfgSetTickerY(bandY);
}

int tickerFont() { return cfgTickerFont(); }

void tickerSetFont(int i) {
  if (i < 0 || i >= FONT_COUNT) return;
  cfgSetTickerFont(i);
  tickerBegin();  // the height of the band changes with it
  cfgSetTickerY(bandY);
}

const char *tickerFontName() { return face->name; }

const char *tickerText() { return cfgTickerText(); }

void tickerSetText(const char *s) {
  cfgSetTickerText(s);
  tickerBegin();
}

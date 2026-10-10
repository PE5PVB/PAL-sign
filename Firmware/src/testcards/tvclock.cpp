// TV clock: a full-picture analogue clock on black, white hour markers
// and hands with round ends; the second hand steps once a second. The
// look and every proportion follow the "TV Klok" of depips.nl (Karel,
// MIT licence): radius 160, markers 26 x 10, hands 0.72/0.92/0.88 of
// the radius, all scaled by min(width, height) / 400 x 0.9. Two changes
// on request: the centre dot is solid, and the hands run on further
// past the centre (HAND_TAIL).
//
// Every element is a capsule (a thick line with round caps), so a row
// crosses each one in a single span; the spans are worked out at four
// heights inside the row and the coverage added up, which anti-aliases
// the edges both ways. That costs far more than a row's 64 us, so it is
// not done per row: the picture only changes once a second, and the
// next second's clock face is drawn ahead, a row at a time in the
// producer's spare time (Pattern::background), into one of two 4-bit
// buffers in the ROM cards' run table (romBorrowScratch(), idle while
// this card is on). The buffers swap on the second; renderRow() only
// expands the bytes.
//
// Aligned left: the gap between the clock and the left picture edge
// equals the gap above and below it. The first and second text line go
// to the right of it, at the top or the bottom (option bit 0). The
// clock and the background take their own colours (CFG_COLOR_CLOCK_FACE
// and _BACK), checked once a picture.
#include <Arduino.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "../clock.h"
#include "../gfx.h"
#include "../language.h"
#include "../pattern.h"
#include "../rompattern.h"
#include "../settings.h"
#include "../textoverlay.h"

// BT.601 625-line pixels are 59/54 as wide as a picture line is high.
static const float PIXEL_ASPECT = 59.0f / 54.0f;
static const float CENTRE_Y = 288.0f;  // picture lines
static const float SCALE = (576.0f / 400.0f) * 0.9f;
static const float RADIUS = 160.0f * SCALE;
// The marker tips reach R - 4 plus their round end of 5: R + 1.
static const float EXTENT = 161.0f * SCALE;
// BT.601 active picture starts at sample 9 of 720; the left gap equals
// the gap above, CENTRE_Y - EXTENT lines.
static const float CENTRE_X = 9.0f + ((CENTRE_Y - EXTENT) + EXTENT) / PIXEL_ASPECT;  // pixels
static const float DOT_OUTER = 9.0f * SCALE;

static const int SUB_ROWS = 4;
// #f2f2f2 on black: Y 16 + 0.949 x 219 = 224, in four equal parts.
static const int SUB_WEIGHT = 52;

static const int MARKERS = 12;
static const int HANDS = 3;   // hour, minute, second
// How far each hand runs on past the centre; depips.nl uses 8.
static const float HAND_TAIL = 24.0f * SCALE;
static const int SHAPES = MARKERS + HANDS;

// In 1/16 picture line, relative to the centre; y grows downwards.
struct Capsule {
  int16_t x1, y1, x2, y2, r;
};
static Capsule shapes[SHAPES];

// The lines (relative to the centre) a capsule can touch.
static inline bool crossesRow(const Capsule &c, int rowRel) {
  const int top = ((c.y1 < c.y2 ? c.y1 : c.y2) - c.r) / 16 - 2;
  const int bottom = ((c.y1 > c.y2 ? c.y1 : c.y2) + c.r) / 16 + 2;
  return rowRel >= top && rowRel <= bottom;
}

static void makeBar(Capsule *c, float angleDeg, float length, float width, float offset) {
  const float a = (angleDeg - 90.0f) * (float)M_PI / 180.0f;
  const float ux = cosf(a), uy = sinf(a);
  const float x1 = ux * offset, y1 = uy * offset;
  const float x2 = ux * (offset + length), y2 = uy * (offset + length);
  const float r = width * 0.5f;
  c->x1 = (int16_t)lroundf(x1 * 16.0f);
  c->y1 = (int16_t)lroundf(y1 * 16.0f);
  c->x2 = (int16_t)lroundf(x2 * 16.0f);
  c->y2 = (int16_t)lroundf(y2 * 16.0f);
  c->r = (int16_t)lroundf(r * 16.0f);
}

// The span a disk covers at height y, or false.
static bool diskSpan(float cx, float cy, float r, float y, float *lo, float *hi) {
  const float dy = y - cy;
  if (dy >= r || dy <= -r) return false;
  const float d = r * r - dy * dy;
  if (d <= 0.0f) return false;
  const float e = sqrtf(d);
  *lo = cx - e;
  *hi = cx + e;
  return true;
}

// A capsule in floats with its direction worked out, once per row for
// the capsules the row crosses, not once per sub-row.
struct Prepared {
  float x1, y1, x2, y2, r;
  float tx, ty, len;
  float invAcross, invAlong;  // 1 / -ty and 1 / tx, 0 when that axis is level
};

static void prepare(const Capsule &c, Prepared *p) {
  p->x1 = c.x1 / 16.0f;
  p->y1 = c.y1 / 16.0f;
  p->x2 = c.x2 / 16.0f;
  p->y2 = c.y2 / 16.0f;
  p->r = c.r / 16.0f;
  const float dx = p->x2 - p->x1, dy = p->y2 - p->y1;
  p->len = sqrtf(dx * dx + dy * dy);
  p->tx = p->len > 1e-6f ? dx / p->len : 0.0f;
  p->ty = p->len > 1e-6f ? dy / p->len : 0.0f;
  p->invAcross = (p->ty > 1e-9f || p->ty < -1e-9f) ? -1.0f / p->ty : 0.0f;
  p->invAlong = (p->tx > 1e-9f || p->tx < -1e-9f) ? 1.0f / p->tx : 0.0f;
}

// One axis of the rectangle: k x + c0 within [mn, mx] narrows [rl, rh].
static inline void clipAxis(float k, float invK, float c0, float mn, float mx, float *rl, float *rh) {
  if (invK == 0.0f) {
    if (c0 < mn || c0 > mx) *rh = *rl;  // empty
    return;
  }
  float p = (mn - c0) * invK, q = (mx - c0) * invK;
  if (k < 0.0f) { const float t = p; p = q; q = t; }
  if (p > *rl) *rl = p;
  if (q < *rh) *rh = q;
}

// A capsule is convex, so the union of its two end disks and its
// rectangle at height y is one span.
static bool capsuleSpan(const Prepared &c, float y, float *lo, float *hi) {
  float l = 1e9f, h = -1e9f, a, b;
  if (diskSpan(c.x1, c.y1, c.r, y, &a, &b)) { l = a; h = b; }
  if (diskSpan(c.x2, c.y2, c.r, y, &a, &b)) {
    if (a < l) l = a;
    if (b > h) h = b;
  }
  if (c.len > 1e-6f) {
    // Across the bar: |-ty (x - x1) + tx (y - y1)| <= r; along it:
    // 0 <= tx (x - x1) + ty (y - y1) <= len.
    const float dy = y - c.y1;
    float rl = -1e9f, rh = 1e9f;
    clipAxis(-c.ty, c.invAcross, c.ty * c.x1 + c.tx * dy, -c.r, c.r, &rl, &rh);
    clipAxis(c.tx, c.invAlong, -c.tx * c.x1 + c.ty * dy, 0.0f, c.len, &rl, &rh);
    if (rl < rh) {
      if (rl < l) l = rl;
      if (rh > h) h = rh;
    }
  }
  if (l >= h) return false;
  *lo = l;
  *hi = h;
  return true;
}

// The part of the picture the clock can reach, with a margin.
// Even, so a byte of the buffer is one Cb,Y,Cr,Y pixel pair.
static const int X_FIRST = ((int)(CENTRE_X - EXTENT / PIXEL_ASPECT) - 2) & ~1;
static const int X_COUNT = 392;  // even: two pixels per byte
static const int Y_FIRST = (int)(CENTRE_Y - EXTENT) - 2;
static const int Y_COUNT = 422;
static const int ROW_BYTES = X_COUNT / 2;
static const uint32_t PICTURE_BYTES = (uint32_t)ROW_BYTES * Y_COUNT;

static uint8_t *pictures[2];   // in the borrowed table; null when it is too small
// The two text lines live in the borrowed table too, after the pictures:
// some 400 bytes of RAM the build does not have to spare.
static TextOverlay *idLine, *subLine;

// Coverage 0..15 to colour, from the background to the clock colour;
// also in the borrowed table.
struct Palette {
  uint8_t y[16], cb[16], cr[16];
  uint32_t backWord;           // the background as a Cb,Y,Cr,Y word
  uint32_t faceRgb, backRgb;   // what it was worked out from
};
static Palette *pal;

static void paletteUpdate() {
  if (!pal) return;
  const uint32_t face = cfgCardColor(-1, CFG_COLOR_CLOCK_FACE);
  const uint32_t back = cfgCardColor(-1, CFG_COLOR_CLOCK_BACK);
  if (face == pal->faceRgb && back == pal->backRgb) return;
  const YCbCr f = yccFromRgb(face), b = yccFromRgb(back);
  for (int n = 0; n < 16; n++) {
    pal->y[n] = (uint8_t)((b.Y * (15 - n) + f.Y * n + 7) / 15);
    pal->cb[n] = (uint8_t)((b.Cb * (15 - n) + f.Cb * n + 7) / 15);
    pal->cr[n] = (uint8_t)((b.Cr * (15 - n) + f.Cr * n + 7) / 15);
  }
  pal->backWord = (uint32_t)b.Cb | ((uint32_t)b.Y << 8) | ((uint32_t)b.Cr << 16) |
                  ((uint32_t)b.Y << 24);
  pal->faceRgb = face;
  pal->backRgb = back;
}

// The text boxes: right of the clock, level with its top or its bottom.
// 34 rows a line keeps the two close together; each line is centred on
// its own ink, so the gap between them is 34 rows less the letter height.
static const int TEXT_X0 = 476, TEXT_X1 = 702;
static const int TEXT_H = 34, TEXT_GAP = 0;
static int front = 0;          // the buffer renderRow() reads
static int64_t frontSecond = -1;
static int64_t backSecond = -1;  // the second the other buffer is being drawn for
static int backRow = 0;          // next row to draw there; Y_COUNT = ready

// The hands for one moment of local time.
static void setHands(const ClockTime &t) {
  const float hourA = (float)(t.hour % 12) * 30.0f + (float)t.min * 0.5f;
  const float minA = (float)t.min * 6.0f + (float)t.sec * 0.1f;
  const float secA = (float)t.sec * 6.0f;
  Capsule *h = &shapes[MARKERS];
  makeBar(&h[0], hourA, RADIUS * 0.72f + HAND_TAIL - 8.0f * SCALE, 9.0f * SCALE, -HAND_TAIL);
  makeBar(&h[1], minA, RADIUS * 0.92f + HAND_TAIL - 8.0f * SCALE, 6.0f * SCALE, -HAND_TAIL);
  makeBar(&h[2], secA, RADIUS * 0.88f + HAND_TAIL - 8.0f * SCALE, 2.5f * SCALE, -HAND_TAIL);
}

// The local time one second on, carried through minutes and hours.
static void nextSecond(ClockTime *t) {
  if (++t->sec < 60) return;
  t->sec = 0;
  if (++t->min < 60) return;
  t->min = 0;
  t->hour = (t->hour + 1) % 24;
}

// Adds count x SUB_WEIGHT x the covered fraction to pixels a..b (in
// pixels of the clock box, a < b); a pixel wholly inside is written.
static void fillStretch(uint8_t *cov, float a, float b, int count) {
  if (count <= 0 || b <= 0.0f || a >= (float)X_COUNT) return;
  if (a < 0.0f) a = 0.0f;
  if (b > (float)X_COUNT) b = (float)X_COUNT;
  const float w = (float)(count * SUB_WEIGHT);
  const int i0 = (int)a, i1 = (int)b;
  if (i0 == i1) {
    cov[i0] += (uint8_t)((b - a) * w + 0.5f);
    return;
  }
  cov[i0] += (uint8_t)(((float)(i0 + 1) - a) * w + 0.5f);
  const uint8_t full = (uint8_t)(count * SUB_WEIGHT);
  for (int i = i0 + 1; i < i1; i++) cov[i] = full;
  if (i1 < X_COUNT) cov[i1] += (uint8_t)((b - (float)i1) * w + 0.5f);
}

// One sub-row's covered stretches, disjoint and in order, in pixels.
struct SubRow {
  float lo[SHAPES + 1], hi[SHAPES + 1];
  int n;
};

// Draws box row r (picture row Y_FIRST + r) of the current shapes into
// out: ROW_BYTES of 4-bit coverage, low nibble first.
static void drawRow(int r, uint8_t *out) {
  uint8_t cov[X_COUNT];
  memset(cov, 0, sizeof(cov));
  const int rowRel = Y_FIRST + r - (int)CENTRE_Y;

  Prepared crossing[SHAPES];
  int nc = 0;
  for (int s = 0; s < SHAPES; s++) {
    if (crossesRow(shapes[s], rowRel)) prepare(shapes[s], &crossing[nc++]);
  }

  const float toPx = 1.0f / PIXEL_ASPECT;
  const float x0 = CENTRE_X - (float)X_FIRST;
  SubRow sub[SUB_ROWS];
  for (int k = 0; k < SUB_ROWS; k++) {
    SubRow &sr = sub[k];
    sr.n = 0;
    const float yy = (float)rowRel + ((float)k + 0.5f) / SUB_ROWS;
    float lo[SHAPES + 1], hi[SHAPES + 1];
    int n = 0;
    for (int s = 0; s < nc; s++) {
      if (capsuleSpan(crossing[s], yy, &lo[n], &hi[n])) n++;
    }
    if (diskSpan(0.0f, 0.0f, DOT_OUTER, yy, &lo[n], &hi[n])) n++;
    if (n == 0) continue;

    // Sort by start, then merge overlaps so a crossing of two hands is
    // not counted twice in one sub-row.
    for (int i = 1; i < n; i++) {
      const float a = lo[i], b = hi[i];
      int j = i - 1;
      while (j >= 0 && lo[j] > a) { lo[j + 1] = lo[j]; hi[j + 1] = hi[j]; j--; }
      lo[j + 1] = a;
      hi[j + 1] = b;
    }
    int m = 0;
    for (int i = 1; i < n; i++) {
      if (lo[i] <= hi[m]) {
        if (hi[i] > hi[m]) hi[m] = hi[i];
      } else {
        m++;
        lo[m] = lo[i];
        hi[m] = hi[i];
      }
    }
    m++;
    for (int i = 0; i < m; i++) {
      sr.lo[sr.n] = x0 + lo[i] * toPx;
      sr.hi[sr.n++] = x0 + hi[i] * toPx;
    }
  }

  // Each sub-row's edges are already in order: merge the four lists;
  // the running count is how many sub-rows are covered between edges.
  int idx[SUB_ROWS] = {0};
  int count = 0;
  float prevX = 0.0f;
  for (;;) {
    int best = -1;
    float bx = 0.0f;
    for (int k = 0; k < SUB_ROWS; k++) {
      if (idx[k] >= 2 * sub[k].n) continue;
      const float x = (idx[k] & 1) ? sub[k].hi[idx[k] >> 1] : sub[k].lo[idx[k] >> 1];
      if (best < 0 || x < bx) {
        best = k;
        bx = x;
      }
    }
    if (best < 0) break;
    if (count > 0 && bx > prevX) fillStretch(cov, prevX, bx, count);
    count += (idx[best] & 1) ? -1 : 1;
    idx[best]++;
    prevX = bx;
  }

  // 0..208 to 0..15.
  const int full = SUB_ROWS * SUB_WEIGHT;
  for (int i = 0; i < ROW_BYTES; i++) {
    const int a = (cov[2 * i] * 15 + full / 2) / full;
    const int b = (cov[2 * i + 1] * 15 + full / 2) / full;
    out[i] = (uint8_t)(a | (b << 4));
  }
}

// Starts drawing the other buffer for the second after `now`.
static void startBack(int64_t now, ClockTime t) {
  nextSecond(&t);
  setHands(t);
  backSecond = now + 1;
  backRow = 0;
}

// One row of the next picture; whether more are waiting.
static bool tvClockBackground() {
  if (!pictures[0] || backRow >= Y_COUNT) return false;
  drawRow(backRow, pictures[front ^ 1] + (uint32_t)backRow * ROW_BYTES);
  backRow++;
  return backRow < Y_COUNT;
}

static void tvClockTick() {
  if (!pictures[0]) return;
  paletteUpdate();
  ClockTime t;
  const int64_t now = clockGet(&t);
  if (now == frontSecond) return;
  if (backSecond == now && backRow >= Y_COUNT) {
    front ^= 1;
    frontSecond = now;
    startBack(now, t);
    return;
  }
  // Not drawn in time, or the clock was set: draw this very second
  // next; the old picture stays up until it is done.
  if (backSecond != now) {
    setHands(t);
    backSecond = now;
    backRow = 0;
  }
}

// The light half of the init: the text layout only.
static void tvClockReflow() {
  if (!idLine) return;
  const int top = Y_FIRST + 2;                  // level with the clock's top
  const int bottom = Y_FIRST + Y_COUNT - 2;     // and its bottom
  int idY, subY;
  if (cfgCardOption(-1) & 1) {
    subY = bottom - TEXT_H;
    idY = subY - TEXT_GAP - TEXT_H;
  } else {
    idY = top;
    subY = idY + TEXT_H + TEXT_GAP;
  }
  textPrepare(idLine, cfgTextIdShown(), TEXT_X0, TEXT_X1, idY, idY + TEXT_H);
  textPrepare(subLine, cfgTextSubShown(), TEXT_X0, TEXT_X1, subY, subY + TEXT_H);
}

static void tvClockInit() {
  const float len = 26.0f * SCALE;
  for (int i = 0; i < MARKERS; i++) {
    makeBar(&shapes[i], (float)i * 30.0f, len, 10.0f * SCALE, RADIUS - len - 4.0f * SCALE);
  }
  uint32_t bytes = 0;
  uint8_t *scratch = romBorrowScratch(&bytes);
  if (bytes < 2 * PICTURE_BYTES) {
    pictures[0] = pictures[1] = nullptr;
    return;
  }
  pictures[0] = scratch;
  pictures[1] = scratch + PICTURE_BYTES;
  if (bytes >= 2 * PICTURE_BYTES + 2 * sizeof(TextOverlay) + sizeof(Palette)) {
    idLine = (TextOverlay *)(scratch + 2 * PICTURE_BYTES);
    subLine = idLine + 1;
    pal = (Palette *)(subLine + 1);
    pal->faceRgb = pal->backRgb = 0xFFFFFFFFu;   // forces the first update
    paletteUpdate();
  } else {
    pictures[0] = pictures[1] = nullptr;
    idLine = subLine = nullptr;
    pal = nullptr;
    return;
  }
  tvClockReflow();

  // The first picture in one go, so it is there from the first row on
  // (a card switch is under its black band anyway).
  ClockTime t;
  const int64_t now = clockGet(&t);
  setHands(t);
  front = 0;
  for (int r = 0; r < Y_COUNT; r++) drawRow(r, pictures[0] + (uint32_t)r * ROW_BYTES);
  frontSecond = now;
  startBack(now, t);
}

static void tvClockRenderRow(uint8_t *base, int y) {
  uint32_t *w = (uint32_t *)base;
  const uint32_t back = pal ? pal->backWord : 0x10801080u;
  for (int i = 0; i < SCREEN_W / 2; i++) w[i] = back;
  const int r = y - Y_FIRST;
  if (!pictures[0] || r < 0 || r >= Y_COUNT) return;
  const uint8_t *src = pictures[front] + (uint32_t)r * ROW_BYTES;
  uint8_t *dst = base + 2 * X_FIRST;   // pixel pair X_FIRST / 2
  for (int i = 0; i < ROW_BYTES; i++) {
    const uint8_t b = src[i];
    if (!b) continue;
    const int lo = b & 15, hi = b >> 4;
    const int m = lo > hi ? lo : hi;   // one chroma sample for the pair
    dst[4 * i] = pal->cb[m];
    dst[4 * i + 1] = pal->y[lo];
    dst[4 * i + 2] = pal->cr[m];
    dst[4 * i + 3] = pal->y[hi];
  }
  if (idLine) {
    textDrawRow(base, idLine, y, textInkId());
    textDrawRow(base, subLine, y, textInkSub());
  }
}

const Pattern PATTERN_TVCLOCK = {LANG_CARD_TV_CLOCK, tvClockInit, tvClockRenderRow, tvClockTick,
                                 false, false, tvClockReflow, nullptr, false, false, nullptr, 0,
                                 tvClockBackground};

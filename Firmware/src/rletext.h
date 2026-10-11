// Blends one row of run tokens (tools/contest_rle.py) into a 4:2:2 row.
//
// A solid token writes white outright; an edge byte goes through the
// lut and only ever lifts, so touching glyphs cannot darken each other.
// Chroma goes neutral with it, pulled towards 128 by the same coverage
// the luma edge gets, written at the even x only like fillSpanSolid().
//
// Performance-critical: solid runs are clipped once and poured out as
// one 32-bit store per pixel pair; an edge byte costs a multiply and a
// shift, no division (rleMix255()).
//
// Shared between the contest page and the ticker; forced inline so a
// caller marked __not_in_flash_func keeps this out of flash too.
#ifndef RLETEXT_H
#define RLETEXT_H

#include <stdint.h>
#include <string.h>

// One solid run into the 4:2:2 row. memcpy, because base + qb is only
// word aligned when base is. The ink is a parameter so the ticker can
// letter in any colour.
static inline __attribute__((always_inline)) void rleSolidSpan(uint8_t *base, int a, int b,
                                                               uint8_t y, uint8_t cb, uint8_t cr,
                                                               int screenW) {
  if (a < 0) a = 0;
  if (b > screenW) b = screenW;
  if (a >= b) return;
  if (a & 1) {
    base[(a >> 1) * 4 + 3] = y;
    a++;
  }
  const uint32_t pair =
      (uint32_t)cb | ((uint32_t)y << 8) | ((uint32_t)cr << 16) | ((uint32_t)y << 24);
  uint8_t *p = base + (a >> 1) * 4;
  for (; a + 2 <= b; a += 2, p += 4) {
    memcpy(p, &pair, 4);
  }
  if (a < b) {
    p[0] = cb;
    p[1] = y;
    p[2] = cr;
  }
}

// p + (target - p) * cov / 255, truncated towards zero like a C
// division: x / 255 == (x * 0x8081) >> 23 holds exactly for
// 0 <= x <= 255 * 255, and the sign is handled apart.
static inline __attribute__((always_inline)) uint8_t rleMix255(uint8_t p, int target,
                                                               uint8_t cov) {
  const int n = (target - p) * cov;
  const int q = n >= 0 ? (int)(((uint32_t)n * 0x8081u) >> 23)
                       : -(int)(((uint32_t)-n * 0x8081u) >> 23);
  return (uint8_t)(p + q);
}

// Dark counterpart, for the ring of the transparent ticker: a solid
// token writes black level with neutral chroma; an edge byte pulls the
// luma towards black by its coverage, chroma untouched. The ticker's
// rings are vertically smoothed (tools/make_contest_font.py,
// smooth_glyph()), so an edge byte can be any coverage.
static inline __attribute__((always_inline)) void rleDarkenRow(uint8_t *base, int x0,
                                                               const uint8_t *s,
                                                               const uint8_t *e,
                                                               int screenW) {
  int x = x0;
  while (s < e) {
    x += *s++;
    const uint8_t op = *s++;
    if (op & 0x80) {
      const int len = op & 0x7F;
      rleSolidSpan(base, x, x + len, 16, 128, 128, screenW);
      x += len;
    } else {
      // Clipped once per run. (d * (cov + 1)) >> 8 is d * cov / 255
      // within one code, exact at 0 and 255.
      const int len = op;
      int a = x < 0 ? -x : 0;
      int b = x + len > screenW ? screenW - x : len;
      for (int i = a; i < b; i++) {
        uint8_t *py = &base[2 * (x + i) + 1];
        const int d = *py - 16;
        if (d > 0) *py = (uint8_t)(*py - ((d * (s[i] + 1)) >> 8));
      }
      s += len;
      x += len;
    }
  }
}

// The ink triple is the colour of the lettering: y through the lut, cb
// and cr as the chroma the solid core writes and the edges blend
// towards. chroma false leaves the edges' chroma alone, for a caller
// that knows it is already cb/cr under every edge byte.
static inline __attribute__((always_inline)) void rleBlendRow(uint8_t *base, int x0,
                                                              const uint8_t *s,
                                                              const uint8_t *e,
                                                              const uint8_t *lut,
                                                              uint8_t y, uint8_t cb, uint8_t cr,
                                                              int screenW, bool chroma = true) {
  int x = x0;
  while (s < e) {
    x += *s++;
    const uint8_t op = *s++;
    if (op & 0x80) {
      const int len = op & 0x7F;
      rleSolidSpan(base, x, x + len, y, cb, cr, screenW);
      x += len;
    } else {
      const int len = op;
      int a = x < 0 ? -x : 0;
      int b = x + len > screenW ? screenW - x : len;
      for (int i = a; i < b; i++) {
        const int xx = x + i;
        const uint8_t cov = s[i];
        const uint8_t v = lut[cov];
        uint8_t *q = base + (xx >> 1) * 4;
        if (xx & 1) {
          if (v > q[3]) q[3] = v;
        } else {
          if (v > q[1]) q[1] = v;
          if (!chroma) continue;
          q[0] = rleMix255(q[0], cb, cov);
          q[2] = rleMix255(q[2], cr, cov);
        }
      }
      s += len;
      x += len;
    }
  }
}
#endif  // RLETEXT_H

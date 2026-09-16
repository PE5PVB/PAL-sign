// Blends one row of run tokens (tools/contest_rle.py) into a 4:2:2 row.
//
// A solid token writes white outright; an edge byte goes through the
// lut and only ever lifts, so touching glyphs cannot darken each other.
// Chroma goes neutral with it, pulled towards 128 by the same coverage
// the luma edge gets, written at the even x only like fillSpanSolid().
//
// Performance-critical: solid runs are clipped once and poured out as
// one 32-bit store per pixel pair; the soft ring costs no multiply,
// since every edge byte in a ring stream is exactly 128 (halfway to
// black is (y + 16) >> 1).
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

// Dark counterpart, for the ring of the transparent ticker: a solid
// token writes black level with neutral chroma; an edge byte halves the
// luma towards black, chroma untouched.
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
      const int len = op;
      s += len;   // the bytes are all 128 and are not read
      for (int i = 0; i < len; i++, x++) {
        if ((unsigned)x >= (unsigned)screenW) continue;
        uint8_t *py = &base[2 * x + 1];
        *py = (uint8_t)((*py + 16) >> 1);
      }
    }
  }
}

// The ink triple is the colour of the lettering: y through the lut, cb
// and cr as the chroma the solid core writes and the edges blend
// towards.
static inline __attribute__((always_inline)) void rleBlendRow(uint8_t *base, int x0,
                                                              const uint8_t *s,
                                                              const uint8_t *e,
                                                              const uint8_t *lut,
                                                              uint8_t y, uint8_t cb, uint8_t cr,
                                                              int screenW) {
  int x = x0;
  while (s < e) {
    x += *s++;
    const uint8_t op = *s++;
    if (op & 0x80) {
      const int len = op & 0x7F;
      rleSolidSpan(base, x, x + len, y, cb, cr, screenW);
      x += len;
    } else {
      int len = op;
      for (int i = 0; i < len; i++, x++) {
        const uint8_t v = lut[*s++];
        if ((unsigned)x >= (unsigned)screenW) continue;
        const int qb = (x >> 1) * 4;
        uint8_t *py = &base[qb + ((x & 1) ? 3 : 1)];
        if (v > *py) *py = v;
        if (!(x & 1)) {
          const uint8_t cov = s[-1];
          uint8_t *pb = &base[qb + 0], *pr = &base[qb + 2];
          *pb = (uint8_t)(*pb + ((cb - *pb) * cov) / 255);
          *pr = (uint8_t)(*pr + ((cr - *pr) * cov) / 255);
        }
      }
    }
  }
}
#endif  // RLETEXT_H

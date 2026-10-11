// See interline.h.
#include "interline.h"

#include <math.h>
#include <string.h>

#include "batc_font.h"

static const int LINE_MAX = 32;

void interInkBuild(InterInk *k, YCbCr ink) {
  k->ink = ink;
  for (int i = 0; i < 256; i++) {
    k->y[i] = (uint8_t)(16 + ((ink.Y - 16) * i + 127) / 255);
    k->cb[i] = (uint8_t)(128 + ((ink.Cb - 128) * i) / 255);
    k->cr[i] = (uint8_t)(128 + ((ink.Cr - 128) * i) / 255);
  }
}

static const ContestGlyph *glyphFor(const char **pp) {
  const char *p = *pp;
  const uint8_t b = (uint8_t)*p;
  uint8_t c;
  if (b == 0xC3 && ((uint8_t)p[1] == 0x98 || (uint8_t)p[1] == 0xB8)) {
    c = 0x01;   // slashed O in UTF-8
    *pp = p + 1;
  } else if (b == 0xD8 || b == 0xF8) {
    c = 0x01;   // slashed O in Latin-1
  } else if (b >= 'a' && b <= 'z') {
    c = (uint8_t)(b - 'a' + 'A');
  } else {
    c = b;
  }
  const int n = (int)(sizeof(BATC_FONT) / sizeof(BATC_FONT[0]));
  for (int i = 0; i < n; i++) {
    if (BATC_FONT[i].c == c) return &BATC_FONT[i];
  }
  return &BATC_FONT[0];   // unknown character -> space
}

// One row of a glyph's runs into a coverage row at pen x.
static void glyphRow(const ContestGlyph *g, int r, uint8_t *cov, int x, int width) {
  const uint8_t *s = g->rle + g->off[r], *e = g->rle + g->off[r + 1];
  while (s < e) {
    x += *s++;
    const uint8_t op = *s++;
    const int len = op & 0x7F;
    for (int i = 0; i < len; i++, x++) {
      const uint8_t v = (op & 0x80) ? 255 : *s++;
      if (x >= 0 && x < width && v > cov[x]) cov[x] = v;
    }
  }
}

// The line at full size: glyphs, pens and the ink box.
struct Layout {
  const ContestGlyph *g[LINE_MAX];
  int pen[LINE_MAX];
  int n, top, bot, left, right;
};

static bool layOut(const char *s, Layout *L) {
  L->n = 0;
  int width = 0;
  for (const char *p = s; *p && L->n < LINE_MAX; p++) {
    L->g[L->n] = glyphFor(&p);
    L->pen[L->n] = width;
    width += L->g[L->n]->adv;
    L->n++;
  }
  L->top = 32767; L->bot = -32768; L->left = 32767; L->right = -32768;
  for (int i = 0; i < L->n; i++) {
    const ContestGlyph *g = L->g[i];
    if (!g->h) continue;
    if (g->yoff < L->top) L->top = g->yoff;
    if (g->yoff + g->h > L->bot) L->bot = g->yoff + g->h;
    if (L->pen[i] + g->lsb < L->left) L->left = L->pen[i] + g->lsb;
    if (L->pen[i] + g->lsb + g->w > L->right) L->right = L->pen[i] + g->lsb + g->w;
  }
  return L->bot > L->top && L->right > L->left;
}

// The vertical scale; the horizontal one is squeeze times it.
static float scaleFor(const Layout &L, int boxW, int boxH, float squeeze) {
  const int srcW = L.right - L.left, srcH = L.bot - L.top;
  float k = 1.0f;
  if (srcW * k * squeeze > boxW) k = (float)boxW / ((float)srcW * squeeze);
  if (srcH * k > boxH) k = (float)boxH / (float)srcH;
  return k;
}

bool interLineLayout(const char *s, int boxW, int boxH, uint8_t *cov, uint8_t *end, int *w,
                     int *h, float squeeze) {
  *w = *h = 0;
  Layout L;
  if (!layOut(s, &L)) return false;
  const int srcW = L.right - L.left, srcH = L.bot - L.top;
  const float k = scaleFor(L, boxW, boxH, squeeze), kx = k * squeeze;
  const int outW = (int)ceilf(srcW * kx), outH = (int)ceilf(srcH * k);

  // One source row of coverage and one row of float sums, after the map.
  uint8_t *srcRow = cov + outW * outH;
  float *acc = (float *)(((uintptr_t)(srcRow + srcW) + 3) & ~(uintptr_t)3);
  if ((uint8_t *)(acc + srcW) > end) return false;

  const float inv = 1.0f / k, invX = 1.0f / kx;   // source pixels per output pixel
  for (int oy = 0; oy < outH; oy++) {
    for (int x = 0; x < srcW; x++) acc[x] = 0.0f;
    const float y0 = oy * inv, y1 = (oy + 1) * inv;
    for (int sy = (int)floorf(y0); sy < (int)ceilf(y1) && sy < srcH; sy++) {
      const float wy = fminf(y1, (float)(sy + 1)) - fmaxf(y0, (float)sy);
      if (wy <= 0.0f) continue;
      memset(srcRow, 0, srcW);
      for (int i = 0; i < L.n; i++) {
        const ContestGlyph *g = L.g[i];
        const int r = sy + L.top - g->yoff;
        if (!g->h || r < 0 || r >= g->h) continue;
        glyphRow(g, r, srcRow, L.pen[i] + g->lsb - L.left, srcW);
      }
      for (int x = 0; x < srcW; x++) acc[x] += wy * srcRow[x];
    }
    uint8_t *out = cov + oy * outW;
    for (int ox = 0; ox < outW; ox++) {
      const float x0 = ox * invX, x1 = (ox + 1) * invX;
      float sum = 0.0f;
      for (int sx = (int)floorf(x0); sx < (int)ceilf(x1) && sx < srcW; sx++) {
        const float wx = fminf(x1, (float)(sx + 1)) - fmaxf(x0, (float)sx);
        if (wx > 0.0f) sum += wx * acc[sx];
      }
      const float v = sum * k * kx;
      out[ox] = (uint8_t)(v > 255.0f ? 255 : (int)(v + 0.5f));
    }
  }
  *w = outW;
  *h = outH;
  return true;
}

void interDrawRow(uint8_t *base, int x0, const uint8_t *cov, int w, const InterInk *k) {
  for (int i = 0; i < w; i++) {
    const uint8_t c = cov[i];
    if (!c) continue;
    const int x = x0 + i;
    base[2 * x + 1] = k->y[c];
    if ((x & 1) == 0) {
      base[2 * x] = k->cb[c];
      base[2 * x + 2] = k->cr[c];
    }
  }
}

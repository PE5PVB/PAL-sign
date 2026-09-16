// Shared drawing primitives for test patterns. Header-only and force
// inlined: run tens of thousands of times per frame.
//
// Frame format: 720x576, 4:2:2 YCbCr, packed per pixel pair as
// Cb, Y0, Cr, Y1, four bytes for every two pixels.
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#define ALWAYS_INLINE __attribute__((always_inline)) inline

static const int SCREEN_W = 720;
static const int SCREEN_H = 576;

struct YCbCr {
  uint8_t Y, Cb, Cr;
};

// Common levels. BT.601 puts Y in 16..235 and neutral chroma at 128.
static const YCbCr GFX_WHITE = {235, 128, 128};
static const YCbCr GFX_BLACK = {16, 128, 128};

// sRGB (packed 0xRRGGBB) to studio YCbCr, with the numbers of to_422()
// in tools/convert_image.py (BT.601), so a lettering colour picked on
// the PC lands exactly as it lands in a converted photograph. Called
// when lettering is laid out, never per row.
inline YCbCr yccFromRgb(uint32_t rgb) {
  const float r = (float)((rgb >> 16) & 0xFF) / 255.0f;
  const float g = (float)((rgb >> 8) & 0xFF) / 255.0f;
  const float b = (float)(rgb & 0xFF) / 255.0f;
  const float yf = 0.299f * r + 0.587f * g + 0.114f * b;
  const float cb = 128.0f + 112.0f * (b - yf) / 0.886f;
  const float cr = 128.0f + 112.0f * (r - yf) / 0.701f;
  YCbCr c;
  c.Y = (uint8_t)(16.0f + 219.0f * yf + 0.5f);
  c.Cb = (uint8_t)(cb < 16.0f ? 16.0f : (cb > 240.0f ? 240.0f : cb + 0.5f));
  c.Cr = (uint8_t)(cr < 16.0f ? 16.0f : (cr > 240.0f ? 240.0f : cr + 0.5f));
  return c;
}

// Fills pixels [x0,x1) with one colour, byte by byte and with bounds
// checking. Meant for narrow or oddly aligned spans.
ALWAYS_INLINE void fillSpanSolid(uint8_t *base, int x0, int x1, YCbCr c) {
  if (x0 < 0) x0 = 0;
  if (x1 > SCREEN_W) x1 = SCREEN_W;
  for (int x = x0; x < x1; x++) {
    int qb = (x >> 1) * 4;
    base[qb + ((x & 1) ? 3 : 1)] = c.Y;
    if (!(x & 1)) {
      base[qb + 0] = c.Cb;
      base[qb + 2] = c.Cr;
    }
  }
}
#endif  // GFX_H

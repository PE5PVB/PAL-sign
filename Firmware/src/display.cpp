// The ST7789P3 status display. See display.h for what it is for.
//
// Offset registers: the ST7789 has frame memory for 240x320, this panel
// is 76x284, a window inside it, so every CASET/RASET needs a fixed
// shift. Not in any datasheet; colstart/rowstart/BGR/invert live in
// config.h and can be nudged over the serial port.
//
// SPI clock: CFG_DISPLAY_SPI_HZ is well under this panel's ceiling; the
// transfer is spread over spare time and never has a deadline to meet.
#include "display.h"

#include <string.h>

#include <Arduino.h>

#include <hardware/gpio.h>
#include <hardware/spi.h>

#include "board.h"
#include "config.h"
#include "display_bg.h"
#include "display_font12.h"
#include "display_font16.h"
#include "display_font28.h"
#include "language.h"

#if CFG_DISPLAY_ON

// --- the panel ---------------------------------------------------------

// ST7789 commands, only the ones used here.
static const uint8_t CMD_SWRESET = 0x01;
static const uint8_t CMD_SLPOUT = 0x11;
static const uint8_t CMD_INVOFF = 0x20;
static const uint8_t CMD_INVON = 0x21;
static const uint8_t CMD_DISPON = 0x29;
static const uint8_t CMD_CASET = 0x2A;
static const uint8_t CMD_RASET = 0x2B;
static const uint8_t CMD_RAMWR = 0x2C;
static const uint8_t CMD_MADCTL = 0x36;
static const uint8_t CMD_COLMOD = 0x3A;
static const uint8_t CMD_NORON = 0x13;

// MADCTL bits, from the ST7789 register description.
static const uint8_t MAD_MY = 0x80;   // row address order
static const uint8_t MAD_MX = 0x40;   // column address order
static const uint8_t MAD_MV = 0x20;   // row/column exchange, so landscape
static const uint8_t MAD_BGR = 0x08;  // 1 = the panel is BGR and not RGB

// Both ways up in landscape; which is right depends on how the panel is
// fitted, so a key and not a constant.
static const uint8_t MAD_LANDSCAPE_A = MAD_MV | MAD_MX;
static const uint8_t MAD_LANDSCAPE_B = MAD_MV | MAD_MY;

static uint8_t rotation = MAD_LANDSCAPE_A;
static bool bgr = CFG_DISPLAY_BGR;
static int colStart = CFG_DISPLAY_COL_START;
static int rowStart = CFG_DISPLAY_ROW_START;
static bool invert = CFG_DISPLAY_INVERT;
static bool ready = false;
static bool blActiveHigh = CFG_DISPLAY_BL_ACTIVE_HIGH;
static bool blWanted = false;

// Backlight level, 0 to 100, driven by PWM so on/dim/off is one call.
// The PWM frequency is well above what the eye follows, clear of the
// 50 Hz field rate the LED string is also subject to.
static int blLevel = CFG_DISPLAY_BRIGHTNESS;

static void backlight(bool on) {
  blWanted = on;
  int pct = on ? blLevel : 0;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  int duty = pct * 255 / 100;
  // Active low sinks the LED current, so the duty cycle inverts: full on
  // is the pin held low.
  if (!blActiveHigh) duty = 255 - duty;
  analogWriteFreq(20000);
  analogWriteRange(255);
  analogWrite(PIN_DISP_BL, duty);
  // BL sinks real LED current; a GPIO resets at 4 mA, low for a
  // backlight, so ask for the pad's maximum.
  gpio_set_drive_strength(PIN_DISP_BL, GPIO_DRIVE_STRENGTH_12MA);
}

void displaySetBrightness(int percent) {
  blLevel = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  if (ready || blWanted) backlight(blWanted);
}

int displayBrightness() { return blLevel; }

static spi_inst_t *const PORT = (CFG_DISPLAY_SPI_INDEX == 0) ? spi0 : spi1;

static inline void csLow() { gpio_put(PIN_DISP_CS, 0); }
static inline void csHigh() { gpio_put(PIN_DISP_CS, 1); }

static void writeCommand(uint8_t c) {
  gpio_put(PIN_DISP_DC, 0);
  csLow();
  spi_write_blocking(PORT, &c, 1);
  csHigh();
}

static void writeData(const uint8_t *p, size_t n) {
  if (!n) return;
  gpio_put(PIN_DISP_DC, 1);
  csLow();
  spi_write_blocking(PORT, p, n);
  csHigh();
}

static void writeCommandData(uint8_t c, const uint8_t *p, size_t n) {
  writeCommand(c);
  writeData(p, n);
}

// Opens a window; everything written after this lands inside it and
// wraps by itself, so the transfer can be cut into pieces of any size.
static void openWindow(int x0, int y0, int x1, int y1) {
  int cx0 = x0 + colStart, cx1 = x1 + colStart;
  int cy0 = y0 + rowStart, cy1 = y1 + rowStart;
  uint8_t a[4];

  a[0] = (uint8_t)(cx0 >> 8); a[1] = (uint8_t)cx0;
  a[2] = (uint8_t)(cx1 >> 8); a[3] = (uint8_t)cx1;
  writeCommandData(CMD_CASET, a, 4);

  a[0] = (uint8_t)(cy0 >> 8); a[1] = (uint8_t)cy0;
  a[2] = (uint8_t)(cy1 >> 8); a[3] = (uint8_t)cy1;
  writeCommandData(CMD_RASET, a, 4);

  writeCommand(CMD_RAMWR);
  // Chip select stays down for the whole run: lifted between two pieces
  // the panel would take the next one as a new command.
  gpio_put(PIN_DISP_DC, 1);
  csLow();
}

static void closeWindow() { csHigh(); }

static uint8_t madctlValue() { return (uint8_t)(rotation | (bgr ? MAD_BGR : 0)); }

static void applyOrientation() {
  const uint8_t v = madctlValue();
  writeCommandData(CMD_MADCTL, &v, 1);
  writeCommand(invert ? CMD_INVON : CMD_INVOFF);
}

// --- what is on the screen --------------------------------------------

// RGB565, byte swapped: the panel wants the high byte first and the
// RP2350 is little endian, so the transfer is a straight memory copy
// with no per-pixel work.
static inline uint16_t rgb565(int r, int g, int b) {
  uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  return (uint16_t)((v >> 8) | (v << 8));
}

static const uint16_t COL_TEXT = rgb565(255, 255, 255);
// Deep yellow, so the one line that says WHAT is on screen stands apart
// from the three that say how.
static const uint16_t COL_TITLE = rgb565(190, 160, 0);
static const uint16_t COL_DIM = rgb565(150, 150, 150);
// Green lifted into the blue: pure blue is very dark on this panel.
static const uint16_t COL_RULE = rgb565(0, 96, 255);
static const uint16_t COL_TITLE_RULE = rgb565(0, 200, 60);
static const uint16_t COL_BACK = rgb565(0, 0, 0);
static const uint16_t COL_MARK = rgb565(255, 40, 40);
// A badge that is ON is reversed: a solid light block with the label
// knocked out. Not full white, which glares next to the softer text
// colours; off stays a grey outline and grey label.
static const uint16_t COL_BADGE_ON = rgb565(195, 195, 195);
static const uint16_t COL_BADGE_OFF = rgb565(80, 80, 80);

// Where each miniature column begins in the picture row, closing entry
// so thumbX[i+1] is where it ends. Worked out once: (i*720)/DISP_THUMB_W
// is a real division and 101 of those per row would cost too much.
static uint16_t thumbX[DISP_THUMB_W + 1];

// Built by averaging, not by picking one sample: a column covers 7.1
// picture samples and a band 7.6 rows, so a single sample per cell
// could land a thin grid line at full white or miss it. Sums are kept
// in the picture's own Y, Cb, Cr, so the RGB conversion happens once
// per column, not once per sample.
static uint32_t accY[DISP_THUMB_W], accCb[DISP_THUMB_W], accCr[DISP_THUMB_W];
static uint16_t accCount[DISP_THUMB_W];

// accY is on the four-sample scale (see displaySampleRow()), so its own
// division happens once, at the flush.
static int accRows = 0;
static int accBand = -1;
// True until the first row of a band has been written, so that row can
// assign instead of add and the zeroing memsets disappear.
static bool accFirst = true;


// True while a card draws the miniature itself; see displayThumbHold().
static bool thumbHeld = false;

// Which quarter of the miniature this picture refreshes; see
// displaySampleRow().
static int samplePhase = 0;

// Written by the producer while it builds rows, read by the transfer,
// both on core0, so no race. Can still tear between two different
// cards, which bandsDone below stops (a second buffer would settle it
// outright at the cost of another 15 kB of RAM).
static uint16_t thumb[DISP_THUMB_H][DISP_THUMB_W];

// Which bands have been built since the last card change, one bit
// each; nothing is sent until all 76 are in, so old and new card never
// share the screen.
static uint32_t bandsDone[3];
static bool thumbDirty = false;

static bool thumbComplete() {
  return bandsDone[0] == 0xFFFFFFFFu && bandsDone[1] == 0xFFFFFFFFu &&
         (bandsDone[2] & 0xFFFu) == 0xFFFu;  // 76 bands: 32 + 32 + 12
}

static void thumbReset() {
  bandsDone[0] = bandsDone[1] = bandsDone[2] = 0;
  thumbDirty = true;  // a different picture, sent once whole
  accBand = -1;
}

// The text, to the right of the miniature.
static const int TEXT_X = DISP_THUMB_W + 3;
static const int RULE_X = DISP_THUMB_W + 1;  // centred in the gap, black either side
static const int TEXT_W = DISP_W - TEXT_X;
static const int TEXT_LINES = 2;

// The three-column gap: two black columns either side of the rule.
// Written once by displaySplashDone(), redrawn by drawRule() whenever
// candidate/menu mode's full-panel background has painted over it.
static void drawRule() {
  openWindow(RULE_X - 1, 0, RULE_X + 1, DISP_H - 1);
  uint16_t row[3] = {COL_BACK, COL_RULE, COL_BACK};
  for (int y = 0; y < DISP_H; y++) spi_write_blocking(PORT, (const uint8_t *)row, sizeof(row));
  closeWindow();
}

// Three bands, not four equal lines: the card name gets the 28 point
// smooth face, the other two lines and the badges keep bitmap faces.
// 34 + 26 + 16 = 76 fills the panel exactly; the static_assert below
// guards it, since a mismatch draws a line off the bottom edge.
static const int TITLE_H = 34;
static const int INFO_H = 26;
static const int BADGE_H = 16;
static_assert(TITLE_H + INFO_H + BADGE_H == DISP_H, "the three bands must fill the panel");

// Band 0 and 1 are text, band 2 is the badges.
static const int BAND_H[3] = {TITLE_H, INFO_H, BADGE_H};
static const int BAND_TOP[3] = {0, TITLE_H, TITLE_H + INFO_H};

// One text line at a time is drawn here and sent; the whole area would
// be another 27 kB. Sized for the tallest line.
static uint16_t strip[TITLE_H][TEXT_W];

// --- the badges --------------------------------------------------------
//
// Short labels in a frame along the bottom: lit when on, dark grey when
// not. Carry state not visible in the picture, teletext and the
// measurement lines above all. Labels are fixed words, so the row is
// always the same width and an active badge never shifts the others.
static const int BADGE_COUNT = 7;
// One column of air either side of a label: two columns would leave
// only 3.8 px between six badges and the row would read as one block;
// one column leaves 6.2 px.
static const int BADGE_PAD = 1;

// Room for the longest label plus terminator; strncpy in
// displaySetBadge() truncates silently past this.
static const int BADGE_LABEL_MAX = 10;

static char badgeLabel[BADGE_COUNT][BADGE_LABEL_MAX];
static bool badgeOn[BADGE_COUNT];
static char badgeSentLabel[BADGE_COUNT][BADGE_LABEL_MAX];
static bool badgeSentOn[BADGE_COUNT];

// Drawn in two runs: the identification goes dark grey when the card on
// screen has it switched off, the card number beside it does not.
static char infoLeft[16];
static char infoRight[32];
static char infoLeftSent[16];
static char infoRightSent[32];
static bool infoDim = false;
static bool infoDimSent = false;

static char lineText[TEXT_LINES][40];
static char lineSent[TEXT_LINES][40];
static bool lineDirty[3];   // the two text bands plus the badge band

// --- text --------------------------------------------------------------
//
// Both faces are smooth: one alpha byte per pixel instead of one bit, so
// an edge can be a part shade. Costs nothing extra at draw time, the
// blend is the same work either way.

static const SmoothGlyph *smoothFor(const SmoothFont &f, char c) {
  for (int i = 0; i < f.count; i++) {
    if (f.glyphs[i].ch == (uint8_t)c) return &f.glyphs[i];
  }
  return nullptr;  // a space carries no ink, so it is not in the table
}

// The widest digit of a face: every digit on the status lines draws to
// this width, so a running clock does not jump sideways at a 1.
static int digitWidth(const SmoothFont &f) {
  int w = 0;
  for (char c = '0'; c <= '9'; c++) {
    const SmoothGlyph *g = smoothFor(f, c);
    if (g && g->adv > w) w = g->adv;
  }
  return w;
}

static int smoothAdvance(const SmoothFont &f, char c, bool fixedDigits) {
  if (fixedDigits && c >= '0' && c <= '9') return digitWidth(f);
  const SmoothGlyph *g = smoothFor(f, c);
  return g ? g->adv : f.space;
}

// Mixes one colour into another in RGB565; the 5/6/5 channels must be
// unpacked first, and byte-swapped back first too since everything
// here is stored high byte first for the panel.
static inline uint16_t swap565(uint16_t v) {
  return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t blend565(uint16_t back, uint16_t fore, uint8_t a) {
  if (a == 0) return back;
  if (a == 255) return fore;
  const uint16_t bk = swap565(back), fg16 = swap565(fore);
  const int br = (bk >> 11) & 0x1F, bg = (bk >> 5) & 0x3F, bb = bk & 0x1F;
  const int fr = (fg16 >> 11) & 0x1F, fgn = (fg16 >> 5) & 0x3F, fb = fg16 & 0x1F;
  // Written so the numerator is never negative, or C's truncation would
  // round a darker-than-background blend the wrong way.
  const int r = (br * (255 - a) + fr * a + 127) / 255;
  const int g = (bg * (255 - a) + fgn * a + 127) / 255;
  const int b = (bb * (255 - a) + fb * a + 127) / 255;
  return swap565((uint16_t)((r << 11) | (g << 5) | b));
}

// One badge; an empty label leaves it out of the row.
void displaySetBadge(int i, const char *label, bool on) {
  if (!ready || i < 0 || i >= BADGE_COUNT) return;
  const char *src = label ? label : "";
  strncpy(badgeLabel[i], src, sizeof(badgeLabel[i]) - 1);
  badgeLabel[i][sizeof(badgeLabel[i]) - 1] = 0;
  badgeOn[i] = on;
}

void displaySetInfo(const char *left, const char *right, bool dim) {
  if (!ready) return;
  strncpy(infoLeft, left ? left : "", sizeof(infoLeft) - 1);
  infoLeft[sizeof(infoLeft) - 1] = 0;
  strncpy(infoRight, right ? right : "", sizeof(infoRight) - 1);
  infoRight[sizeof(infoRight) - 1] = 0;
  infoDim = dim;
}

void displaySetLine(int i, const char *text) {
  if (!ready || i < 0 || i >= TEXT_LINES) return;
  const char *src = text ? text : "";

  // Cut to what fits, here and not at the caller: the font is
  // proportional, so only this file can measure it in pixels.
  int w = 0, n = 0;
  const int room = (int)sizeof(lineText[i]) - 1;
  while (src[n] && n < room) {
    const int adv = (i == 0) ? smoothAdvance(DISPFONT28, src[n], false)
                             : smoothAdvance(DISPFONT16, src[n], true);
    if (w + adv > TEXT_W) break;
    w += adv;
    n++;
  }
  memcpy(lineText[i], src, n);
  lineText[i][n] = 0;
}

static int drawRun(const SmoothFont &f, const char *s, int baseline, int boxH,
                   uint16_t colour, bool fixedDigits, int pen);

// The baseline of a band: the ink of the face centred in it.
static int bandBaseline(const SmoothFont &f, int boxH) {
  const int band = f.inkUp + f.inkDown;
  const int slack = boxH - band;
  return f.inkUp + (slack >= 0 ? slack / 2 : -((-slack + 1) / 2));
}

// Draws one line into strip[], clipped at the right edge. The box is not
// always as tall as the face (the status lines get 16 rows, the face
// asks for 17); the baseline centres the ink band in the box and the
// shortfall, if any, comes off the bottom first.
static void drawSmooth(const SmoothFont &f, const char *s, int top, int boxH,
                       uint16_t colour, bool fixedDigits) {
  // The strip starts as a copy of the background picture, not a fill;
  // stored row major and as wide as the text area, so this is one memcpy.
  static_assert(DISPLAY_BG_W == TEXT_W && DISPLAY_BG_H == DISP_H,
                "the background has to be exactly the text area");
  memcpy(strip, &DISPLAY_BG[(size_t)top * TEXT_W],
         (size_t)boxH * TEXT_W * sizeof(uint16_t));
  const int baseline = bandBaseline(f, boxH);
  drawRun(f, s, baseline, boxH, colour, fixedDigits, 0);
}

// One run of text at a given pen position, into a strip already filled
// in. Returns where the pen ended up, so a second run can follow the
// first in another colour.
static int drawRun(const SmoothFont &f, const char *s, int baseline, int boxH,
                   uint16_t colour, bool fixedDigits, int pen) {
  for (const char *p = s; *p; p++) {
    const int adv = smoothAdvance(f, *p, fixedDigits);
    if (pen + adv > TEXT_W) break;
    const SmoothGlyph *g = smoothFor(f, *p);
    if (g) {
      // A digit sits centred in its fixed width, or a narrow 1 stands
      // hard against its neighbour.
      const bool digit = fixedDigits && *p >= '0' && *p <= '9';
      const int lead = digit ? (adv - g->adv) / 2 : 0;
      const uint8_t *px = &f.pixels[g->off];
      for (int r = 0; r < g->h; r++) {
        const int yy = baseline - g->dy + r;
        if (yy < 0 || yy >= boxH) continue;
        for (int c = 0; c < g->w; c++) {
          const uint8_t a = px[r * g->w + c];
          if (!a) continue;
          const int xx = pen + lead + g->dx + c;
          if (xx < 0 || xx >= TEXT_W) continue;
          uint16_t *d = &strip[yy][xx];
          *d = (a == 255) ? colour : blend565(*d, colour, a);
        }
      }
    }
    pen += adv;
  }
  return pen;
}

// How wide a badge is: its label plus the air inside and the frame.
static int badgeWidth(const char *label) {
  int w = 0;
  for (const char *p = label; *p; p++) w += smoothAdvance(DISPFONT12, *p, false);
  return w + 2 * BADGE_PAD + 2;
}

// Baseline that centres the labels in use, worked out over them so it
// cannot drift from what is really drawn.
static int badgeBaseline() {
  int up = 0, down = 0;
  for (int i = 0; i < BADGE_COUNT; i++) {
    for (const char *p = badgeLabel[i]; *p; p++) {
      const SmoothGlyph *g = smoothFor(DISPFONT12, *p);
      if (!g) continue;
      if (g->dy > up) up = g->dy;
      if (g->h - g->dy > down) down = g->h - g->dy;
    }
  }
  if (up == 0) up = DISPFONT12.ascent;
  return up + (BADGE_H - (up + down)) / 2;
}

// The info band: card number, then identification behind it, dark grey
// when this card does not draw that text on screen.
static void drawInfo() {
  memcpy(strip, &DISPLAY_BG[(size_t)BAND_TOP[1] * TEXT_W],
         (size_t)INFO_H * TEXT_W * sizeof(uint16_t));

  const int baseline = bandBaseline(DISPFONT16, INFO_H);
  int pen = drawRun(DISPFONT16, infoLeft, baseline, INFO_H, COL_DIM, true, 0);
  if (infoRight[0]) {
    pen += smoothAdvance(DISPFONT16, ' ', false) * 2;
    drawRun(DISPFONT16, infoRight, baseline, INFO_H, infoDim ? COL_BADGE_OFF : COL_DIM,
            true, pen);
  }
}

// The row of badges along the bottom.
static void drawBadges() {
  memcpy(strip, &DISPLAY_BG[(size_t)BAND_TOP[2] * TEXT_W],
         (size_t)BADGE_H * TEXT_W * sizeof(uint16_t));

  // Justified across the band: first badge against the left edge, last
  // against the right, the rest of the width shared equally between the
  // gaps. Worked out from the running total, not by adding a gap each
  // time, so rounding cannot pile up and the last badge lands exactly on
  // the edge.
  int widths[BADGE_COUNT];
  int total = 0;
  int count = 0;
  for (int i = 0; i < BADGE_COUNT; i++) {
    if (!badgeLabel[i][0]) continue;
    widths[count] = badgeWidth(badgeLabel[i]);
    total += widths[count];
    count++;
  }
  if (count == 0) return;

  int slack = TEXT_W - total;
  if (slack < 0) slack = 0;

  int before = 0;   // the widths of the badges already placed
  int placed = 0;
  for (int i = 0; i < BADGE_COUNT; i++) {
    if (!badgeLabel[i][0]) continue;
    const int w = widths[placed];
    const int x = (count > 1) ? before + (slack * placed) / (count - 1)
                              : (TEXT_W - w) / 2;
    before += w;
    placed++;
    const bool on = badgeOn[i];
    const uint16_t colour = on ? COL_BADGE_ON : COL_BADGE_OFF;

    if (on) {
      // Reverse: the whole badge becomes a solid block, the label is
      // knocked out of it.
      for (int r = 0; r < BADGE_H; r++) {
        for (int c = 0; c < w && x + c < TEXT_W; c++) strip[r][x + c] = colour;
      }
    } else {
      // The frame, one pixel all round.
      for (int c = 0; c < w && x + c < TEXT_W; c++) {
        strip[0][x + c] = colour;
        strip[BADGE_H - 1][x + c] = colour;
      }
      for (int r = 0; r < BADGE_H; r++) {
        if (x < TEXT_W) strip[r][x] = colour;
        if (x + w - 1 < TEXT_W) strip[r][x + w - 1] = colour;
      }
    }

    // Baseline from the labels themselves, not the face metrics: those
    // count descenders no badge label has, which would sit the row too
    // high. In reverse the label is drawn in the background colour,
    // blended against the white block.
    const uint16_t ink = on ? COL_BACK : colour;
    const int baseline = badgeBaseline();
    int pen = x + 1 + BADGE_PAD;
    for (const char *p = badgeLabel[i]; *p; p++) {
      const SmoothGlyph *g = smoothFor(DISPFONT12, *p);
      const int adv = g ? g->adv : DISPFONT12.space;
      if (g) {
        const uint8_t *px = &DISPFONT12.pixels[g->off];
        for (int r = 0; r < g->h; r++) {
          const int yy = baseline - g->dy + r;
          if (yy < 0 || yy >= BADGE_H) continue;
          for (int c = 0; c < g->w; c++) {
            const uint8_t a = px[r * g->w + c];
            if (!a) continue;
            const int xx = pen + g->dx + c;
            if (xx < 0 || xx >= TEXT_W) continue;
            uint16_t *d = &strip[yy][xx];
            *d = (a == 255) ? ink : blend565(*d, ink, a);
          }
        }
      }
      pen += adv;
    }
  }
}

// --- the miniature -----------------------------------------------------

static inline int clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// BT.601 studio range to full range RGB, integer coefficients: 255/219
// for luma, the R-Y and B-Y factors of BT.601 scaled by 255/224.
static inline uint16_t ycbcrToRgb565(int Y, int Cb, int Cr) {
  const int y = 298 * (Y - 16);
  const int u = Cb - 128;
  const int v = Cr - 128;
  return rgb565(clip255((y + 409 * v + 128) >> 8),
                clip255((y - 100 * u - 208 * v + 128) >> 8),
                clip255((y + 516 * u + 128) >> 8));
}

// In RAM, like everything else the producer touches per row: this runs
// on 76 rows of every picture.
static void __not_in_flash_func(flushBand)() {
  if (accBand < 0 || accRows == 0) return;
  uint16_t *out = thumb[accBand];
  for (int i = 0; i < DISP_THUMB_W; i++) {
    const uint16_t n = accCount[i];
    if (!n) return;  // this band never got a sample
    (void)n;
    // accY is on the four-sample scale, divided here and only here.
    const int Y = (int)(accY[i] / (uint32_t)(accRows * 4));
    out[i] = ycbcrToRgb565(Y, (int)(accCb[i] / n), (int)(accCr[i] / n));
  }
  bandsDone[accBand >> 5] |= 1u << (accBand & 31);
}

void __not_in_flash_func(displaySampleRow)(const uint8_t *base, int y, bool mayWork) {
  if (!ready || thumbHeld) return;
  // Field one only: a test card is still, so the other field carries
  // nothing new, at half the work.
  if (y & 1) return;

  const int band = (y * DISP_THUMB_H) / 576;
  if (band >= DISP_THUMB_H) return;

  // A quarter of the miniature per picture, in turn: doing every band
  // every picture would cost margin a card change cannot spare, and the
  // miniature only goes out once every CFG_DISPLAY_THUMB_EVERY pictures
  // anyway. Every row of the bands that ARE picked, though: a single
  // sampled row per band would make the average just that row.
  if ((band % CFG_DISPLAY_THUMB_EVERY) != samplePhase) return;

  // "Not now" comes first and takes the band with it: the mayWork flag
  // can turn true partway down a picture, so a band caught mid-flip
  // would otherwise close over too few rows and still be marked done.
  if (!mayWork) {
    accBand = -1;
    accRows = 0;
    return;
  }

  // A new band: the one before it is finished, flush it.
  //
  // First row assigns, it does not add: zeroing four arrays compiles to
  // four memsets, and memset lives in flash, evicting the XIP cache on
  // every photograph row (memcpy right beside it does the same). The
  // ticker fell into this trap twice.
  if (band != accBand) {
    flushBand();
    accBand = band;
    accRows = 0;
    accFirst = true;
  }

  // Four samples a column, four consecutive words: a column spans seven
  // or eight samples, every second one taken, which comes to four either
  // way, and in 4:2:2 two samples are one 32 bit word (Cb Y0 Cr Y1). The
  // row buffer is 4-aligned and the offset a multiple of 4, so the reads
  // are aligned words, not byte-addressed.
  for (int i = 0; i < DISP_THUMB_W; i++) {
    const uint32_t *w = (const uint32_t *)(base + (thumbX[i] >> 1) * 4);
    const int shiftY = (thumbX[i] & 1) ? 24 : 8;  // which Y of the pair
    uint32_t sy = 0, sb = 0, sr = 0;
    for (int k = 0; k < 4; k++) {
      const uint32_t v = w[k];
      sy += (v >> shiftY) & 0xFFu;
      sb += v & 0xFFu;
      sr += (v >> 16) & 0xFFu;
    }
    const int n = 4;
    if (accFirst) {
      accY[i] = sy;
      accCb[i] = sb;
      accCr[i] = sr;
      accCount[i] = (uint16_t)n;
    } else {
      accY[i] += sy;
      accCb[i] += sb;
      accCr[i] += sr;
      accCount[i] += (uint16_t)n;
    }
  }
  accFirst = false;
  accRows++;
}

// Both defined further down, with the transfer.
static void abortJob();
static bool thumbJobRunning();
// The radial gradient, defined further down with the splash; candidate
// mode reuses it, stretched by nearest neighbour sampling (fine on a
// gradient).
static inline uint16_t splashBgPixel(int x, int y);

void displayThumbHold(bool hold) {
  // A transfer of the miniature in flight has to be cut off: left
  // running it would resume out of an array the new card has since
  // refilled, splitting old and new card across the panel.
  if (thumbJobRunning()) abortJob();
  thumbHeld = hold;
  thumbReset();
  if (hold) {
    // The card draws the miniature itself, so it is complete by
    // definition.
    bandsDone[0] = bandsDone[1] = bandsDone[2] = 0xFFFFFFFFu;
  }
}

void displayThumbPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (!ready || x < 0 || y < 0 || x >= DISP_THUMB_W || y >= DISP_THUMB_H) return;
  thumb[y][x] = rgb565(r, g, b);
}

void displayThumbCopyRow(int to, int from) {
  if (!ready || to < 0 || from < 0 || to >= DISP_THUMB_H || from >= DISP_THUMB_H) return;
  memcpy(thumb[to], thumb[from], sizeof(thumb[0]));
}

// --- the transfer as a job ---------------------------------------------

// Region 0 is the miniature, 1 to TEXT_LINES the text lines. -1 is idle.
static int jobRegion = -1;
static uint32_t jobPos = 0;
static uint32_t jobLen = 0;
static const uint8_t *jobSrc = nullptr;
static uint32_t frameCount = 0;

static bool startRegion(int region) {
  if (region == 0) {
    thumbDirty = false;
    openWindow(0, 0, DISP_THUMB_W - 1, DISP_THUMB_H - 1);
    jobSrc = (const uint8_t *)thumb;
    jobLen = sizeof(thumb);
    return true;
  }
  const int i = region - 1;
  if (i >= 3) return false;
  if (!lineDirty[i]) return false;
  if (i == 0) {
    drawSmooth(DISPFONT28, lineText[0], BAND_TOP[0], TITLE_H, COL_TITLE, false);
    // The line under the title, on the last row of its band: the 28
    // point face's ink reaches row 31 of 34, so row 32 is a gap and 33
    // carries the line.
    for (int x = 0; x < TEXT_W; x++) strip[TITLE_H - 1][x] = COL_TITLE_RULE;
    memcpy(lineSent[0], lineText[0], sizeof(lineSent[0]));
  } else if (i == 1) {
    drawInfo();
    memcpy(infoLeftSent, infoLeft, sizeof(infoLeftSent));
    memcpy(infoRightSent, infoRight, sizeof(infoRightSent));
    infoDimSent = infoDim;
  } else {
    drawBadges();
    for (int b = 0; b < BADGE_COUNT; b++) {
      memcpy(badgeSentLabel[b], badgeLabel[b], sizeof(badgeSentLabel[b]));
      badgeSentOn[b] = badgeOn[b];
    }
  }
  lineDirty[i] = false;
  openWindow(TEXT_X, BAND_TOP[i], DISP_W - 1, BAND_TOP[i] + BAND_H[i] - 1);
  jobSrc = (const uint8_t *)strip;
  // Only this band's own rows: strip[] is sized for the tallest one.
  jobLen = (unsigned)BAND_H[i] * TEXT_W * sizeof(uint16_t);
  return true;
}

// --- candidate mode: the whole panel while the knob offers a card ------
//
// While the knob offers a card, the panel drops the miniature, lines,
// info band and badges, and shows just the name, large and centred,
// over the splash's own gradient, with a hint and a position count.
// One static frame: a slide in/out was tried and read poorly.
//
// One row at a time, not a whole panel buffer (284x76x2 = 43 kB is
// more RAM than this board has to spare): candRow[] is the row in
// flight, regenerated whenever the job crosses into a new row.
static uint16_t candRow[DISP_W];

// The name laid out once, when it changes: glyphs of DISPFONT28 and
// their pen positions, so a row only walks this list instead of
// re-measuring 76 times.
static const int CAND_GLYPH_MAX = 32;
static const SmoothGlyph *candGlyph[CAND_GLYPH_MAX];
static int16_t candGlyphX[CAND_GLYPH_MAX];
static int candGlyphN = 0;
static int candBaseline = 0;   // row the name's baseline sits on, within its own band

// The edit cursor's column, captured regardless of whether that
// character has a glyph (a space is a valid cursor position but has
// none). candCursorAdv is 0 when no cursor is laid out this call.
static int16_t candCursorX = 0, candCursorAdv = 0;

// The hint line, DISPFONT12 like the badges, in the bottom CAND_HINT_H
// rows.
static const int CAND_HINT_H = BADGE_H;
static const char *const CAND_HINT_TEXT = LANG_DISP_HINT_CANDIDATE;
static const char *const BUSY_HINT_TEXT = LANG_DISP_HINT_BUSY;
static const SmoothGlyph *candHintGlyph[CAND_GLYPH_MAX];
static int16_t candHintGlyphX[CAND_GLYPH_MAX];
static int candHintGlyphN = 0;
static int candHintBaseline = 0;   // row within the hint band
static const int CAND_NAME_H = DISP_H - CAND_HINT_H;   // rows the name gets

// The position count, "3/24", right aligned in the hint band;
// Firmware.ino works it out and hands it in with the name.
static char candCount[16];
static const SmoothGlyph *candCountGlyph[CAND_GLYPH_MAX];
static int16_t candCountGlyphX[CAND_GLYPH_MAX];
static int candCountGlyphN = 0;

// 97: candName also carries the on-device menu's edit value. The
// longest field is the ticker text (TICKER_MAX_CHARS = 96) plus the
// terminator.
static char candName[97], candNameSent[97];
static bool candOn = false, candOnSent = false;

// A PC-tool operation (download/erase/upload): shares candName's job
// and glyph machinery, but checked first in displayFrameHook() so it
// pre-empts candidate/menu mode, since it reports a hardware state
// (the picture is black) rather than something the knob offers.
static char busyText[40], busyTextSent[40];
static bool busyOn = false, busyOnSent = false;

static int candJobRow = -1;   // which row is queued, -1 = idle
static uint32_t candJobPos = 0;   // bytes sent of candRow so far

// Lays out candName into candGlyph[], boxH high, in DISPFONT28. The
// on-device menu reuses this for its value line (candOn and menuOn are
// mutually exclusive, so sharing the storage is safe).
//
// leftAlign starts the line at column 0 instead of centring it.
// cursorCharIndex is the character of candName the edit cursor sits
// under, or -1 for none; its column is captured in the same pass.
// skip is how many leading characters this pass drops, growing by one
// and retrying until the cursor's column fits within DISP_W.
static void layoutValueText(int boxH, bool leftAlign, int cursorCharIndex) {
  int skip = 0;
  int w = 0;
  for (;;) {
    candGlyphN = 0;
    candCursorAdv = 0;
    w = 0;
    int charIndex = skip;
    for (const char *p = candName + skip; *p && candGlyphN < CAND_GLYPH_MAX; p++, charIndex++) {
      const int adv = smoothAdvance(DISPFONT28, *p, false);
      if (w + adv > DISP_W) break;
      if (charIndex == cursorCharIndex) {
        candCursorX = (int16_t)w;
        candCursorAdv = (int16_t)adv;
      }
      const SmoothGlyph *g = smoothFor(DISPFONT28, *p);
      if (g) {
        candGlyph[candGlyphN] = g;
        candGlyphX[candGlyphN] = (int16_t)w;
        candGlyphN++;
      }
      w += adv;
    }
    if (candCursorAdv > 0 || cursorCharIndex < 0 || skip >= cursorCharIndex) break;
    skip++;
  }
  // Centred on the panel width, same rule splashLine() applies.
  // leftAlign skips this and pins the line to column 0.
  const int pen0 = leftAlign ? 0 : ((w < DISP_W) ? (DISP_W - w) / 2 : 0);
  for (int i = 0; i < candGlyphN; i++) candGlyphX[i] = (int16_t)(candGlyphX[i] + pen0);
  if (candCursorAdv > 0) candCursorX = (int16_t)(candCursorX + pen0);
  candBaseline = bandBaseline(DISPFONT28, boxH);
}

// Lays out text into candHintGlyph[], the bottom-left band; shared
// storage between candidate mode and the menu, same as layoutValueText().
static void layoutHintText(const char *text) {
  candHintGlyphN = 0;
  int w = 0;
  for (const char *p = text; *p && candHintGlyphN < CAND_GLYPH_MAX; p++) {
    const int adv = smoothAdvance(DISPFONT12, *p, false);
    if (w + adv > DISP_W) break;
    const SmoothGlyph *g = smoothFor(DISPFONT12, *p);
    if (g) {
      candHintGlyph[candHintGlyphN] = g;
      candHintGlyphX[candHintGlyphN] = (int16_t)w;
      candHintGlyphN++;
    }
    w += adv;
  }
  const int pen0 = (w < DISP_W) ? (DISP_W - w) / 2 : 0;
  for (int i = 0; i < candHintGlyphN; i++) candHintGlyphX[i] = (int16_t)(candHintGlyphX[i] + pen0);
  candHintBaseline = bandBaseline(DISPFONT12, CAND_HINT_H);
}

// The "3/24": same band and face as the hint, right aligned.
static void candidateLayoutCount() {
  candCountGlyphN = 0;
  int w = 0;
  for (const char *p = candCount; *p && candCountGlyphN < CAND_GLYPH_MAX; p++) {
    const int adv = smoothAdvance(DISPFONT12, *p, true);   // digits fixed width, no jump
    const SmoothGlyph *g = smoothFor(DISPFONT12, *p);
    if (g) {
      const bool digit = *p >= '0' && *p <= '9';
      const int lead = digit ? (adv - g->adv) / 2 : 0;
      candCountGlyph[candCountGlyphN] = g;
      candCountGlyphX[candCountGlyphN] = (int16_t)(w + lead);
      candCountGlyphN++;
    }
    w += adv;
  }
  const int pen0 = DISP_W - w;
  for (int i = 0; i < candCountGlyphN; i++) candCountGlyphX[i] = (int16_t)(candCountGlyphX[i] + pen0);
}

// --- the on-device menu -------------------------------------------------
//
// Same full-panel background and row job as candidate mode, reused
// rather than duplicated (the two never show at once). New here is a
// second text line above the value, small and dim, naming the item.
static bool menuOn = false, menuOnSent = false;
static const int MENU_LABEL_H = 18;
static const int MENU_VALUE_H = CAND_NAME_H - MENU_LABEL_H;

// The bargraph's geometry. MENU_BAR_TEXT_W is room for the number
// beside it: the widest reading, hue's -127..127, is 29 px, so 56
// leaves margin.
static const int MENU_BAR_TEXT_W = 56;
static const int MENU_BAR_MARGIN = 8;
static const int MENU_BAR_X0 = MENU_BAR_MARGIN;
static const int MENU_BAR_X1 = DISP_W - MENU_BAR_MARGIN - MENU_BAR_TEXT_W;
static const int MENU_BAR_H = 14;
static const int MENU_BAR_Y0 = (MENU_VALUE_H - MENU_BAR_H) / 2;
static const int MENU_BAR_Y1 = MENU_BAR_Y0 + MENU_BAR_H;

static char menuLabel[24], menuLabelSent[24];
static const SmoothGlyph *menuLabelGlyph[CAND_GLYPH_MAX];
static int16_t menuLabelGlyphX[CAND_GLYPH_MAX];
static int menuLabelGlyphN = 0;
static int menuLabelBaseline = 0;

// Hint and count share candHintGlyph[]/candCountGlyph[] with candidate
// mode; menuHint is the menu's own source string.
static char menuHint[40], menuHintSent[40];
static char menuCountSent[16];
static bool menuLeftAlign = false, menuLeftAlignSent = false;
// Character index the edit cursor sits under, or -1 for none; always
// the real position. menuCursorVisible is the blink's own on/off.
static int menuCursorIndex = -1, menuCursorIndexSent = -1;
static bool menuCursorVisible = false, menuCursorVisibleSent = false;

// The bargraph, in place of layoutValueText()'s centred number for a
// slider or an analogue control. lo/hi/value are the control's range
// and reading; on draws the bar (off everywhere else in the menu, and
// for candidate mode, which never calls this).
static bool menuBarOn = false, menuBarOnSent = false;
static int menuBarLo = 0, menuBarLoSent = 0;
static int menuBarHi = 0, menuBarHiSent = 0;
static int menuBarValue = 0, menuBarValueSent = 0;

// The number beside the bar, DISPFONT16, laid out from candName; own
// glyph list since candGlyph[] etc. are busy with other things on
// screen at the same time.
static const SmoothGlyph *menuBarGlyph[CAND_GLYPH_MAX];
static int16_t menuBarGlyphX[CAND_GLYPH_MAX];
static int menuBarGlyphN = 0;
static int menuBarBaseline = 0;

static void menuLayoutLabel() {
  menuLabelGlyphN = 0;
  int w = 0;
  for (const char *p = menuLabel; *p && menuLabelGlyphN < CAND_GLYPH_MAX; p++) {
    const int adv = smoothAdvance(DISPFONT12, *p, false);
    if (w + adv > DISP_W) break;
    const SmoothGlyph *g = smoothFor(DISPFONT12, *p);
    if (g) {
      menuLabelGlyph[menuLabelGlyphN] = g;
      menuLabelGlyphX[menuLabelGlyphN] = (int16_t)w;
      menuLabelGlyphN++;
    }
    w += adv;
  }
  const int pen0 = (w < DISP_W) ? (DISP_W - w) / 2 : 0;
  for (int i = 0; i < menuLabelGlyphN; i++) menuLabelGlyphX[i] = (int16_t)(menuLabelGlyphX[i] + pen0);
  menuLabelBaseline = bandBaseline(DISPFONT12, MENU_LABEL_H);
}

// Lays out candName into menuBarGlyph[], DISPFONT16, starting just
// after the bar rather than centred, so it sits fixed beside it like a
// scale's own figure.
static void layoutBarText() {
  menuBarGlyphN = 0;
  int w = MENU_BAR_X1 + MENU_BAR_MARGIN;
  for (const char *p = candName; *p && menuBarGlyphN < CAND_GLYPH_MAX; p++) {
    const int adv = smoothAdvance(DISPFONT16, *p, false);
    if (w + adv > DISP_W) break;
    const SmoothGlyph *g = smoothFor(DISPFONT16, *p);
    if (g) {
      menuBarGlyph[menuBarGlyphN] = g;
      menuBarGlyphX[menuBarGlyphN] = (int16_t)w;
      menuBarGlyphN++;
    }
    w += adv;
  }
  menuBarBaseline = bandBaseline(DISPFONT16, MENU_VALUE_H);
}

// One glyph's worth of one row, blended in at x0..x0+g->w; shared by
// the glyph lists drawCandidateRow() walks.
static void blendGlyphRow(uint16_t *out, const SmoothFont &f, const SmoothGlyph *g, int r, int x0,
                          uint16_t colour) {
  const uint8_t *px = &f.pixels[g->off];
  for (int c = 0; c < g->w; c++) {
    const uint8_t a = px[r * g->w + c];
    if (!a) continue;
    const int xx = x0 + c;
    if (xx < 0 || xx >= DISP_W) continue;
    out[xx] = (a == 255) ? colour : blend565(out[xx], colour, a);
  }
}

// Fills one row of the candidate frame: the gradient, then either the
// name (top CAND_NAME_H rows) or the hint band. menuOn splits the top
// into the menu's own label-over-value pair instead.
static void drawCandidateRow(int y, uint16_t *out) {
  for (int x = 0; x < DISP_W; x++) out[x] = splashBgPixel(x, y);
  // The value box's edges, full width, in the title rule colour.
  {
    const int boxTop = menuOn ? MENU_LABEL_H : 0;
    if (y == boxTop || y == CAND_NAME_H - 1) {
      for (int x = 0; x < DISP_W; x++) out[x] = COL_TITLE_RULE;
      return;
    }
  }
  if (menuOn && y < MENU_LABEL_H) {
    for (int i = 0; i < menuLabelGlyphN; i++) {
      const SmoothGlyph *g = menuLabelGlyph[i];
      const int r = y - (menuLabelBaseline - g->dy);
      if (r < 0 || r >= g->h) continue;
      blendGlyphRow(out, DISPFONT12, g, r, menuLabelGlyphX[i] + g->dx, COL_DIM);
    }
    return;
  }
  if (menuOn && menuBarOn && y < CAND_NAME_H) {
    // Outline (COL_DIM), filled portion (COL_TITLE) left, flat background right.
    const int vy = y - MENU_LABEL_H;
    if (vy >= MENU_BAR_Y0 && vy < MENU_BAR_Y1) {
      const bool edgeRow = (vy == MENU_BAR_Y0 || vy == MENU_BAR_Y1 - 1);
      const int span = menuBarHi - menuBarLo;
      int fillX = MENU_BAR_X0;
      if (span > 0) {
        int v = menuBarValue;
        if (v < menuBarLo) v = menuBarLo;
        if (v > menuBarHi) v = menuBarHi;
        fillX = MENU_BAR_X0 + (int)((long)(v - menuBarLo) * (MENU_BAR_X1 - MENU_BAR_X0) / span);
      }
      for (int x = MENU_BAR_X0; x < MENU_BAR_X1; x++) {
        const bool edgeCol = (x == MENU_BAR_X0 || x == MENU_BAR_X1 - 1);
        out[x] = (edgeRow || edgeCol) ? COL_DIM : ((x < fillX) ? COL_TITLE : COL_BACK);
      }
    }
    for (int i = 0; i < menuBarGlyphN; i++) {
      const SmoothGlyph *g = menuBarGlyph[i];
      const int r = vy - (menuBarBaseline - g->dy);
      if (r < 0 || r >= g->h) continue;
      blendGlyphRow(out, DISPFONT16, g, r, menuBarGlyphX[i] + g->dx, COL_TITLE);
    }
    return;
  }
  if (menuOn && y < CAND_NAME_H) {
    const int vy = y - MENU_LABEL_H;
    // Edit cursor bar, a couple of rows under the baseline, drawn
    // before the glyphs so a character on the cursor still reads over it.
    static const int CURSOR_BAR_TOP = 2, CURSOR_BAR_H = 2;
    if (menuCursorVisible && candCursorAdv > 0 && vy >= candBaseline + CURSOR_BAR_TOP &&
        vy < candBaseline + CURSOR_BAR_TOP + CURSOR_BAR_H) {
      for (int xx = candCursorX; xx < candCursorX + candCursorAdv; xx++) {
        if (xx >= 0 && xx < DISP_W) out[xx] = COL_TITLE;
      }
    }
    for (int i = 0; i < candGlyphN; i++) {
      const SmoothGlyph *g = candGlyph[i];
      const int r = vy - (candBaseline - g->dy);
      if (r < 0 || r >= g->h) continue;
      blendGlyphRow(out, DISPFONT28, g, r, candGlyphX[i] + g->dx, COL_TITLE);
    }
    return;
  }
  if (y < CAND_NAME_H) {
    for (int i = 0; i < candGlyphN; i++) {
      const SmoothGlyph *g = candGlyph[i];
      const int r = y - (candBaseline - g->dy);
      if (r < 0 || r >= g->h) continue;
      blendGlyphRow(out, DISPFONT28, g, r, candGlyphX[i] + g->dx, COL_TITLE);
    }
  } else {
    const int hy = y - CAND_NAME_H;
    for (int i = 0; i < candHintGlyphN; i++) {
      const SmoothGlyph *g = candHintGlyph[i];
      const int r = hy - (candHintBaseline - g->dy);
      if (r < 0 || r >= g->h) continue;
      blendGlyphRow(out, DISPFONT12, g, r, candHintGlyphX[i] + g->dx, COL_DIM);
    }
    for (int i = 0; i < candCountGlyphN; i++) {
      const SmoothGlyph *g = candCountGlyph[i];
      const int r = hy - (candHintBaseline - g->dy);
      if (r < 0 || r >= g->h) continue;
      blendGlyphRow(out, DISPFONT12, g, r, candCountGlyphX[i] + g->dx, COL_DIM);
    }
  }
}

static void abortCandidateJob() {
  if (candJobRow >= 0) closeWindow();
  candJobRow = -1;
  candJobPos = 0;
}

// Set around a PC-tool card operation in Firmware.ino, not from the
// per-picture update: text non-null shows it fullscreen, nullptr
// restores the working layout. See displayFrameHook() for how a
// caller outside the normal frame flow (the video-suspended writes in
// photoEraseSlot()/photoUpload()) gets this pushed out immediately.
void displaySetBusy(const char *text) {
  if (!ready) return;
  const bool on = text != nullptr;
  if (on) {
    strncpy(busyText, text, sizeof(busyText) - 1);
    busyText[sizeof(busyText) - 1] = 0;
  } else {
    busyText[0] = 0;
  }
  busyOn = on;
}

// Called once a picture from Firmware.ino's updateDisplayLines(). name
// is the candidate's, or nullptr while nothing is offered; count is
// "3/24", ignored when name is nullptr.
void displaySetCandidate(const char *name, const char *count) {
  if (!ready) return;
  const bool on = name != nullptr;
  if (on) {
    strncpy(candName, name, sizeof(candName) - 1);
    candName[sizeof(candName) - 1] = 0;
    strncpy(candCount, count ? count : "", sizeof(candCount) - 1);
    candCount[sizeof(candCount) - 1] = 0;
  } else {
    candName[0] = 0;
  }
  candOn = on;
}

// Called once a picture from Firmware.ino's updateDisplayLines() while
// the menu is open. label is null while it is not.
void displaySetMenu(const char *label, const char *value, const char *hint, const char *count,
                     bool leftAlign, int cursorIndex, bool cursorVisible) {
  if (!ready) return;
  const bool on = label != nullptr;
  if (on) {
    strncpy(menuLabel, label, sizeof(menuLabel) - 1);
    menuLabel[sizeof(menuLabel) - 1] = 0;
    strncpy(candName, value ? value : "", sizeof(candName) - 1);
    candName[sizeof(candName) - 1] = 0;
    strncpy(menuHint, hint ? hint : "", sizeof(menuHint) - 1);
    menuHint[sizeof(menuHint) - 1] = 0;
    strncpy(candCount, count ? count : "", sizeof(candCount) - 1);
    candCount[sizeof(candCount) - 1] = 0;
    menuLeftAlign = leftAlign;
    menuCursorIndex = cursorIndex;
    menuCursorVisible = cursorVisible;
  } else {
    menuLabel[0] = 0;
  }
  menuOn = on;
}

void displaySetMenuBar(bool on, int lo, int hi, int value) {
  if (!ready) return;
  menuBarOn = on;
  menuBarLo = lo;
  menuBarHi = hi;
  menuBarValue = value;
}

bool displayBusy() { return jobRegion >= 0 || candJobRow >= 0; }

bool displayStep(unsigned bytes) {
  if (candJobRow >= 0) {
    while (bytes) {
      if (candJobPos == 0) drawCandidateRow(candJobRow, candRow);
      uint32_t n = (uint32_t)sizeof(candRow) - candJobPos;
      if (n > bytes) n = bytes;
      spi_write_blocking(PORT, (const uint8_t *)candRow + candJobPos, n);
      candJobPos += n;
      bytes -= n;
      if (candJobPos >= sizeof(candRow)) {
        candJobPos = 0;
        if (++candJobRow >= DISP_H) {
          closeWindow();
          candJobRow = -1;
          return false;
        }
      }
    }
    return true;
  }

  if (jobRegion < 0) return false;

  while (bytes) {
    if (jobPos >= jobLen) {
      closeWindow();
      // On to the next region that has anything to send.
      for (;;) {
        if (++jobRegion > 3) {
          jobRegion = -1;
          return false;
        }
        if (startRegion(jobRegion)) break;
      }
      jobPos = 0;
      continue;
    }
    uint32_t n = jobLen - jobPos;
    if (n > bytes) n = bytes;
    spi_write_blocking(PORT, jobSrc + jobPos, n);
    jobPos += n;
    bytes -= n;
  }
  return true;
}

void displayFrameHook() {
  if (!ready) return;
  frameCount++;
  samplePhase = (samplePhase + 1) % CFG_DISPLAY_THUMB_EVERY;

  if (strcmp(lineText[0], lineSent[0]) != 0) lineDirty[0] = true;
  if (infoDim != infoDimSent || strcmp(infoLeft, infoLeftSent) != 0 ||
      strcmp(infoRight, infoRightSent) != 0) {
    lineDirty[1] = true;
  }
  for (int b = 0; b < BADGE_COUNT; b++) {
    if (badgeOn[b] != badgeSentOn[b] ||
        strcmp(badgeLabel[b], badgeSentLabel[b]) != 0) {
      lineDirty[2] = true;
    }
  }
  // Kept running even while candidate mode covers the panel, so the
  // working layout is ready to resend the moment it returns.

  // Checked before candOn/menuOn: a PC-tool operation reports a
  // hardware fact (the picture is black) that outranks what the knob
  // is doing locally.
  if (busyOn) {
    if (!busyOnSent && jobRegion >= 0) abortJob();
    if (jobRegion >= 0 || candJobRow >= 0) return;  // still going out

    if (busyOnSent && strcmp(busyText, busyTextSent) == 0) return;  // nothing changed
    strncpy(candName, busyText, sizeof(candName) - 1);
    candName[sizeof(candName) - 1] = 0;
    layoutValueText(CAND_NAME_H, false, -1);
    layoutHintText(BUSY_HINT_TEXT);
    candCount[0] = 0;
    candidateLayoutCount();
    strncpy(busyTextSent, busyText, sizeof(busyTextSent) - 1);
    busyTextSent[sizeof(busyTextSent) - 1] = 0;
    strncpy(candNameSent, candName, sizeof(candNameSent) - 1);
    candNameSent[sizeof(candNameSent) - 1] = 0;
    busyOnSent = true;
    openWindow(0, 0, DISP_W - 1, DISP_H - 1);
    candJobRow = 0;
    candJobPos = 0;
    return;
  }

  if (candOn) {
    if (!candOnSent && jobRegion >= 0) abortJob();   // a normal job may be mid flight
    if (jobRegion >= 0 || candJobRow >= 0) return;  // still going out

    if (candOnSent && strcmp(candName, candNameSent) == 0) return;  // nothing changed
    layoutValueText(CAND_NAME_H, false, -1);
    layoutHintText(CAND_HINT_TEXT);
    candidateLayoutCount();
    strncpy(candNameSent, candName, sizeof(candNameSent) - 1);
    candNameSent[sizeof(candNameSent) - 1] = 0;
    candOnSent = true;
    openWindow(0, 0, DISP_W - 1, DISP_H - 1);
    candJobRow = 0;
    candJobPos = 0;
    return;
  }

  if (menuOn) {   // same reasoning as candOn above, sharing its job
    if (!menuOnSent && jobRegion >= 0) abortJob();
    if (jobRegion >= 0 || candJobRow >= 0) return;  // still going out

    if (menuOnSent && strcmp(menuLabel, menuLabelSent) == 0 &&
        strcmp(candName, candNameSent) == 0 && strcmp(menuHint, menuHintSent) == 0 &&
        strcmp(candCount, menuCountSent) == 0 && menuLeftAlign == menuLeftAlignSent &&
        menuCursorIndex == menuCursorIndexSent && menuCursorVisible == menuCursorVisibleSent &&
        menuBarOn == menuBarOnSent && menuBarLo == menuBarLoSent && menuBarHi == menuBarHiSent &&
        menuBarValue == menuBarValueSent) {
      return;  // nothing changed
    }
    menuLayoutLabel();
    if (menuBarOn) {
      layoutBarText();
    } else {
      layoutValueText(MENU_VALUE_H, menuLeftAlign, menuCursorIndex);
    }
    layoutHintText(menuHint);
    candidateLayoutCount();
    strncpy(menuLabelSent, menuLabel, sizeof(menuLabelSent) - 1);
    menuLabelSent[sizeof(menuLabelSent) - 1] = 0;
    strncpy(candNameSent, candName, sizeof(candNameSent) - 1);
    candNameSent[sizeof(candNameSent) - 1] = 0;
    strncpy(menuHintSent, menuHint, sizeof(menuHintSent) - 1);
    menuHintSent[sizeof(menuHintSent) - 1] = 0;
    strncpy(menuCountSent, candCount, sizeof(menuCountSent) - 1);
    menuCountSent[sizeof(menuCountSent) - 1] = 0;
    menuLeftAlignSent = menuLeftAlign;
    menuCursorIndexSent = menuCursorIndex;
    menuCursorVisibleSent = menuCursorVisible;
    menuBarOnSent = menuBarOn;
    menuBarLoSent = menuBarLo;
    menuBarHiSent = menuBarHi;
    menuBarValueSent = menuBarValue;
    menuOnSent = true;
    openWindow(0, 0, DISP_W - 1, DISP_H - 1);
    candJobRow = 0;
    candJobPos = 0;
    return;
  }

  if (candOnSent || menuOnSent || busyOnSent) {
    // Just left candidate, menu or busy mode; the working layout has
    // to redraw in full, since the full panel covered it. The
    // miniature itself was not touched, so thumbDirty alone resends it.
    abortCandidateJob();
    candOnSent = false;
    menuOnSent = false;
    busyOnSent = false;
    drawRule();
    lineDirty[0] = lineDirty[1] = lineDirty[2] = true;
    thumbDirty = true;
  }

  if (jobRegion >= 0) return;  // the last one is still going out

  // Sent the moment it is complete, and on the usual beat after that.
  static bool wasComplete = false;
  const bool complete = thumbComplete();
  const bool justComplete = complete && !wasComplete;
  wasComplete = complete;

  const bool wantThumb =
      complete && (thumbDirty || justComplete || (frameCount % CFG_DISPLAY_THUMB_EVERY) == 0);
  bool wantText = false;
  for (int i = 0; i < 3; i++) wantText |= lineDirty[i];
  if (!wantThumb && !wantText) return;

  // Region 0 is the miniature; skip it by starting at 1 when this
  // picture is not one of the ones that refresh it.
  jobRegion = wantThumb ? 0 : 1;
  jobPos = 0;
  for (;;) {
    if (jobRegion > 3) {
      jobRegion = -1;
      return;
    }
    if (startRegion(jobRegion)) return;
    jobRegion++;
  }
}

// --- finding the offsets -----------------------------------------------

static bool thumbJobRunning() { return jobRegion == 0; }

static void abortJob() {
  if (jobRegion >= 0) closeWindow();
  jobRegion = -1;
  jobPos = 0;
  jobLen = 0;
}

void displayTestCard() {
  if (!ready) return;
  abortJob();
  backlight(true);  // no point drawing this on a dark panel
  // A frame round the edge and a block in every corner: all four
  // corners in view and the frame touching every edge means the
  // offsets are right.
  openWindow(0, 0, DISP_W - 1, DISP_H - 1);
  for (int y = 0; y < DISP_H; y++) {
    uint16_t row[DISP_W];
    for (int x = 0; x < DISP_W; x++) {
      const bool edge = (x == 0) || (y == 0) || (x == DISP_W - 1) || (y == DISP_H - 1);
      const bool corner = (x < 8 || x >= DISP_W - 8) && (y < 8 || y >= DISP_H - 8);
      row[x] = edge ? COL_TEXT : (corner ? COL_MARK : COL_BACK);
    }
    spi_write_blocking(PORT, (const uint8_t *)row, sizeof(row));
  }
  closeWindow();
  // Everything on the panel was just overwritten, so redraw the text
  // even though it did not change.
  for (int i = 0; i < TEXT_LINES; i++) lineSent[i][0] = 1;
  infoLeftSent[0] = 1;
  for (int b = 0; b < BADGE_COUNT; b++) badgeSentLabel[b][0] = 1;
  for (int i = 0; i < 3; i++) lineDirty[i] = true;
}

void displayNudge(int dcol, int drow) {
  colStart += dcol;
  rowStart += drow;
}

void displayFlipOrientation() {
  if (!ready) return;
  abortJob();
  rotation = (rotation == MAD_LANDSCAPE_A) ? MAD_LANDSCAPE_B : MAD_LANDSCAPE_A;
  applyOrientation();
}

void displayToggleBgr() {
  if (!ready) return;
  abortJob();
  bgr = !bgr;
  applyOrientation();
}

void displayToggleInvert() {
  if (!ready) return;
  abortJob();
  invert = !invert;
  applyOrientation();
}

void displayFlipBacklight() {
  if (!ready) return;
  blActiveHigh = !blActiveHigh;
  backlight(true);
}

void displayReport(char *buf, unsigned n) {
  snprintf(buf, n, LANG_DISP_REPORT_FMT,
           colStart, rowStart, madctlValue(), bgr ? LANG_BGR : LANG_RGB, invert ? LANG_ON : LANG_OFF,
           blWanted ? LANG_ON : LANG_OFF, blActiveHigh ? LANG_HIGH : LANG_LOW, (int)CFG_DISPLAY_SPI_HZ,
           (int)CFG_DISPLAY_SPI_INDEX);
}

bool displayOn() { return ready; }

// Whether the splash is still up; the boot progress bar only draws while
// it is, so a late call cannot paint over the working layout.
static bool splashUp = false;

// The splash background is the radial gradient of the text area
// (display_bg.h), stretched over the full panel width by nearest
// neighbour sampling; invisible on a gradient. The lettering blends onto
// it the same way it blends onto the working layout's background.
static inline uint16_t splashBgPixel(int x, int y) {
  return DISPLAY_BG[(size_t)y * DISPLAY_BG_W + (x * DISPLAY_BG_W) / DISP_W];
}

// Where the splash text strip sits; splashLine() copies the background
// from the same place, so the two line up.
static const int SPLASH_X0 = (DISP_W - TEXT_W) / 2;

// One centred line of the splash: the strip is only as wide as the text
// area, so a window centred on the panel and text centred in the strip
// together centre it on screen.
static void splashLine(const SmoothFont &f, const char *s, int top, int boxH,
                       uint16_t colour) {
  for (int y = 0; y < boxH; y++)
    for (int x = 0; x < TEXT_W; x++) strip[y][x] = splashBgPixel(SPLASH_X0 + x, top + y);
  int w = 0;
  for (const char *p = s; *p; p++) w += smoothAdvance(f, *p, false);
  const int pen = (w < TEXT_W) ? (TEXT_W - w) / 2 : 0;
  drawRun(f, s, bandBaseline(f, boxH), boxH, colour, false, pen);
  const int x0 = (DISP_W - TEXT_W) / 2;
  openWindow(x0, top, x0 + TEXT_W - 1, top + boxH - 1);
  spi_write_blocking(PORT, (const uint8_t *)strip, (size_t)boxH * TEXT_W * 2);
  closeWindow();
}

// The three splash lines, pulled out of displayBegin() so
// displayBootResetCountdown() has something to put back if a held
// button lets go before the countdown reaches 0. splashUp itself is set
// by the two callers, not here.
static void drawSplashText() {
  splashLine(DISPFONT16, CFG_VERSION "   \xA9" "2026 PE5PVB", 47, 19, COL_DIM);
  splashLine(DISPFONT16, LANG_SPLASH_TITLE, 29, 19, COL_TITLE_RULE);
  splashLine(DISPFONT28, LANG_SPLASH_NAME, 0, 31, COL_TITLE);
}

void displayBegin() {
  spi_init(PORT, CFG_DISPLAY_SPI_HZ);
  spi_set_format(PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  gpio_set_function(PIN_DISP_SCK, GPIO_FUNC_SPI);
  gpio_set_function(PIN_DISP_MOSI, GPIO_FUNC_SPI);

  static const uint8_t plain[] = {PIN_DISP_CS, PIN_DISP_DC, PIN_DISP_RST, PIN_DISP_BL};
  for (unsigned i = 0; i < sizeof(plain); i++) {
    gpio_init(plain[i]);
    gpio_set_dir(plain[i], GPIO_OUT);
    gpio_put(plain[i], 1);
  }
  backlight(false);  // off until there is something to see

  // Hardware reset. The ST7789 datasheet asks for at least 10 us low;
  // 20 ms either side covers a slow rise on the module's own supply.
  gpio_put(PIN_DISP_RST, 1);
  delay(20);
  gpio_put(PIN_DISP_RST, 0);
  delay(20);
  gpio_put(PIN_DISP_RST, 1);
  delay(120);

  writeCommand(CMD_SWRESET);
  delay(150);
  writeCommand(CMD_SLPOUT);
  delay(120);

  const uint8_t colmod = 0x55;  // 16 bits per pixel, RGB565
  writeCommandData(CMD_COLMOD, &colmod, 1);
  applyOrientation();
  writeCommand(CMD_NORON);
  delay(10);
  writeCommand(CMD_DISPON);
  delay(100);

  for (int i = 0; i <= DISP_THUMB_W; i++) thumbX[i] = (uint16_t)((i * 720) / DISP_THUMB_W);
  thumbX[DISP_THUMB_W] = 720;  // the closing entry, so the last column has an end too
  ready = true;

  // Black first, so nothing of the frame memory shows before the first
  // real picture; blocking is fine, the producer is not running yet.
  openWindow(0, 0, DISP_W - 1, DISP_H - 1);
  for (int y = 0; y < DISP_H; y++) {
    uint16_t row[DISP_W];
    for (int x = 0; x < DISP_W; x++) row[x] = splashBgPixel(x, y);
    spi_write_blocking(PORT, (const uint8_t *)row, sizeof(row));
  }
  closeWindow();

  // First call in setup(), so there is something to look at while the
  // bring-up runs; displaySplashDone() puts the working layout back at
  // the end of it.
  splashUp = true;
  drawSplashText();
  displayBootProgress(0);   // the empty frame, so the bar is there from the start
  backlight(true);
}

// The boot progress bar, in the splash's bottom margin. Drawn whole on
// every call: 220x7 is about 3 kB over SPI, under a millisecond.
static const int BOOT_BAR_W = 220;
static const int BOOT_BAR_H = 7;

void displayBootProgress(int percent) {
  if (!ready || !splashUp) return;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  const int x0 = (DISP_W - BOOT_BAR_W) / 2;
  const int y0 = 66;
  const int fillW = (BOOT_BAR_W - 2) * percent / 100;
  openWindow(x0, y0, x0 + BOOT_BAR_W - 1, y0 + BOOT_BAR_H - 1);
  uint16_t row[BOOT_BAR_W];
  for (int y = 0; y < BOOT_BAR_H; y++) {
    const bool edge = (y == 0) || (y == BOOT_BAR_H - 1);
    for (int x = 0; x < BOOT_BAR_W; x++) {
      if (edge || x == 0 || x == BOOT_BAR_W - 1) {
        row[x] = COL_DIM;
      } else if (x - 1 < fillW) {
        row[x] = COL_TITLE;
      } else {
        // The empty part shows the gradient, so the bar reads as a
        // frame on the background rather than a black slot in it.
        row[x] = splashBgPixel(x0 + x, y0 + y);
      }
    }
    spi_write_blocking(PORT, (const uint8_t *)row, sizeof(row));
  }
  closeWindow();
}

// Counts down a held-button factory reset over the splash. Uses
// splashLine(), the splash's own helper, so it shares its centring and
// background handling.
void displayBootResetCountdown(int secondsLeft) {
  if (!ready || !splashUp) return;
  if (secondsLeft < 0) secondsLeft = 0;
  if (secondsLeft > 9) secondsLeft = 9;
  char digit[2] = {(char)('0' + secondsLeft), 0};
  splashLine(DISPFONT28, digit, 0, 31, COL_TITLE);
  splashLine(DISPFONT16, LANG_SPLASH_RESTORING_DEFAULTS, 29, 19, COL_TITLE_RULE);
  splashLine(DISPFONT16, secondsLeft > 0 ? LANG_SPLASH_RELEASE_TO_CANCEL : LANG_SPLASH_PLEASE_WAIT, 47, 19,
             COL_DIM);
}

void displayBootSplashRestore() {
  if (!ready || !splashUp) return;
  drawSplashText();
}

// Replaces the splash with the working layout: background under the
// text, the rule beside the miniature, clean line caches. Called at the
// end of the bring-up, just before the producer starts.
void displaySplashDone() {
  if (!ready) return;
  splashUp = false;

  openWindow(0, 0, DISP_W - 1, DISP_H - 1);
  uint16_t row[DISP_W];
  for (int x = 0; x < DISP_W; x++) row[x] = COL_BACK;
  for (int y = 0; y < DISP_H; y++) spi_write_blocking(PORT, (const uint8_t *)row, sizeof(row));
  closeWindow();

  // The background of the text area, once; every line redraw copies its
  // own rows out of this same table.
  openWindow(TEXT_X, 0, DISP_W - 1, DISP_H - 1);
  spi_write_blocking(PORT, (const uint8_t *)DISPLAY_BG, sizeof(DISPLAY_BG));
  closeWindow();

  drawRule();

  for (int i = 0; i < TEXT_LINES; i++) {
    lineText[i][0] = 0;
    lineSent[i][0] = 1;  // different, so the first picture draws all four
    lineDirty[i] = true;
  }
}

#else  // CFG_DISPLAY_ON

void displayBegin() {}
void displaySplashDone() {}
void displayBootProgress(int) {}
void displayBootResetCountdown(int) {}
void displayBootSplashRestore() {}
bool displayOn() { return false; }
void displaySampleRow(const uint8_t *, int, bool) {}
void displayFrameHook() {}
bool displayStep(unsigned) { return false; }
bool displayBusy() { return false; }
void displayTestCard() {}
void displayNudge(int, int) {}
void displayFlipOrientation() {}
void displayToggleInvert() {}
void displayToggleBgr() {}
void displayFlipBacklight() {}
void displaySetBrightness(int) {}
int displayBrightness() { return 0; }
void displayThumbHold(bool) {}
void displayThumbPixel(int, int, uint8_t, uint8_t, uint8_t) {}
void displayThumbCopyRow(int, int) {}
void displaySetLine(int, const char *) {}
void displaySetBadge(int, const char *, bool) {}
void displaySetInfo(const char *, const char *, bool) {}
void displaySetCandidate(const char *, const char *) {}
void displaySetBusy(const char *) {}
void displaySetMenu(const char *, const char *, const char *, const char *, bool, int, bool) {}
void displaySetMenuBar(bool, int, int, int) {}
void displayReport(char *buf, unsigned n) { snprintf(buf, n, LANG_NO_DISPLAY_IN_THIS_BUILD); }

#endif

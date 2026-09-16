// EIAJ / IEC 60841 PCM audio in the video signal, the format of the Sony
// PCM-F1/PCM-501ES/PCM-701ES adaptors. Audio from the PCM1808
// (src/pcm1808.h), resampled to 44.1 kHz (src/asrc.h), 3 stereo
// samples a line. Two per-card options (cfgCardOption(), bit 0 and
// bit 1): the 14 bit standard or Sony's 16 bit variant, and 50/15 us
// pre-emphasis on or off.
//
// Format, from IEC 60841:1988 (the international edition of EIAJ
// STC-007, which adds 625/50), the Sony patents EP0048152A1/US4459696A
// and the PCM-F1 manual:
//  - 168 bit periods per line (clause 7.4, fig. 3b): 26 before the data
//    sync "1010", 128 data, one "0", 4 white reference, 5 blank. NRZ,
//    1 = white.
//  - Data block (11.1-11.6): six 14 bit samples L0 R0 L1 R1 L2 R2, P, Q,
//    16 bit CRCC. P = L0^R0^L1^R1^L2^R2. Q = T^6 L0 ^ T^5 R0 ^ T^4 L1 ^
//    T^3 R1 ^ T^2 L2 ^ T R2, T the 14x14 generator matrix of 11.3: T w =
//    (w << 1) with bit 13 fed back into bits 0 and 8, the LFSR of
//    x^14 + x^8 + 1. CRCC = x^16 + x^12 + x^5 + 1 over the 112 data bits
//    with the first 16 inverted (11.5): CRC-16/CCITT-FALSE, init 0xFFFF,
//    MSB first.
//  - Interleave D = 16 lines (11.4, "[An, Bn-3D, An+1-6D, Bn+1-9D,
//    An+2-12D, Bn+2-15D, Pn-18D, Qn-21D]", D = 48 words): the line of
//    block k carries L[k][0], R[k-16][0], L[k-32][1], R[k-48][1],
//    L[k-64][2], R[k-80][2], P of block k-96, Q of block k-112.
//  - One control block a field, ahead of the data (12): 56 bits
//    "1100..." (as 14 bit words 0x3333 0x0CCC 0x3333 0x0CCC), 14 bit
//    ID = 0, 28 bit address = 0, 14 bit control word, CRCC. Control
//    word (Table I, bit 1 = MSB): 1-2 standard ID 00, 11 copy prohibit,
//    12 P, 13 Q, 14 pre-emphasis; 0 = applied / not prohibited.
//  - 16 bit (Appendix B): Q is replaced by an S word with bits 15 and
//    16 of L0 R0 L1 R1 L2 R2 and P, in that order, P being the XOR of
//    the full 16 bit words with its top 14 in the P slot. P only then.
//  - Emphasis (9.2): t1 = 50 us, t2 = 15 us, the pre-emphasis bit tells
//    the decoder to de-emphasise.
//  - Lines (8.3, fig. 4b): "the control data block line shall be located
//    at the 6th line for the first (third) field and 6.5th line for the
//    second (fourth) field. The 294 audio data block lines shall follow."
//    That is control on lines 6 and 319, data 7-300 and 320-613. NOT
//    what this card does, see CTRL_LINE_F1 below.
//  - Timing: bit b starts 36 b / 7 luma pixels after 0H (1728 samples
//    per line / 168 bits = 72/7 samples, 36/7 pixels). The active
//    picture starts 132 pixels after 0H (BT.601: 0H at luminance sample
//    732 of 864), so the data sync starts at active pixel 1.7 and the
//    white reference ends at 706.3. Pixels 0..9 are therefore kept
//    (keepEdges). Measured on a Blackmagic capture the data sync sits
//    ~7 pixels earlier than that; the 132 has not been checked with a
//    scope.
#include <string.h>

#include <Arduino.h>

#include "../adv7391.h"
#include "../asrc.h"
#include "../gfx.h"
#include "../language.h"
#include "../pattern.h"
#include "../pcm1808.h"
#include "../settings.h"

// EXPERIMENT (30 August 2026): IEC 60841 figure 4b puts the control
// block on line 6/319 and the data on 7-300/320-613. A 576 line capture
// (rows = lines 24-311 and 337-624) then misses 17 data lines a field,
// two words of one block each field, which a decoder with P only (the
// OpenScope decoder used for comparison) cannot repair; the same
// decoder plays a real Sony PAL recording from the same capture card
// cleanly, so a real Sony evidently places its lines later than the
// figure. Everything 10 lines down keeps the 294 lines and the control
// block ahead of them, loses only 7 lines a field to the capture (one
// word a block at most, P repairs it), and puts the control block on
// line 16, inside the 7-22 the ADV7391 lets out. Measured: with the
// data on 18-311 OpenScope played the card cleanly, 14 and 16 bit; but
// the ADV7391 blanks lines 311-319 and 624-6 (datasheet figure 104:
// vertical blank "from line 311 through 335", with the VBI-open window
// 320-335 excepted), so the last data line of each field never left
// the board. Hence 17-310 and 330-623 now, one line up. Where a real
// Sony puts its lines is still to be read off a real recording.
//
// Option bit 2, "control line in picture": for decoders that work from
// a capture card and never see the blanking (OpenScope reads its
// control block from the first captured row). Control on 24/337, the
// first rows a DeckLink delivers, data on 25-318 and 338-631; 311-318
// and 624-6 are blanked by the encoder, so the last 8 blocks of each
// field are erasures, within the D = 16 interleave that P repairs
// (consecutive erasures hit one word a block up to 48 blocks). Not
// the standard placement: a real adaptor plays it, correcting all
// the time. Lines past 625 are lines 1-6 of the next frame.
static const uint32_t STD_LINES[6] = {16, 17, 310, 329, 330, 623};
static const uint32_t PIC_LINES[6] = {24, 25, 318, 337, 338, 631};
static uint32_t CTRL_LINE_F1, DATA_FIRST_F1, DATA_LAST_F1;
static uint32_t CTRL_LINE_F2, DATA_FIRST_F2, DATA_LAST_F2;
static bool ctrlInPicture;

static const int ACTIVE_ORIGIN_PX = 132;  // active pixel 0, in pixels after 0H
static const int FIRST_BIT = 26;          // the data sync "1010" starts here
static const int LINE_BITS = 137;         // 4 + 128 + 1 + 4

static const uint32_t CELL_BLACK = 0x10801080u;
static const uint8_t Y_WHITE = 235;

// cfgCardOption() bits.
static const int OPT_16BIT = 1;
static const int OPT_EMPHASIS = 2;
static const int OPT_CTRL_IN_PICTURE = 4;

static const int DEPTH = 113;  // blocks of history the interleave reaches back
static int16_t ringL[DEPTH][3];
static int16_t ringR[DEPTH][3];
// The next block's ring slot, wrapped by hand: DEPTH is not a power
// of two, so slot = counter % DEPTH on a free-running counter would
// jump 16 slots at the 2^32 wrap (~81 h of Sony display) and play
// CRC-valid but wrong samples for ~112 lines.
static int blk;  // 0..DEPTH-1
static bool mode16;
static bool emphasis;

static inline uint16_t q14(int16_t s) { return (uint16_t)((s >> 2) & 0x3FFF); }

static inline uint16_t matT(uint16_t w) {
  return (uint16_t)(((w << 1) & 0x3FFF) ^ ((w & 0x2000) ? 0x0101 : 0));
}

// CRC-16/CCITT-FALSE, a 14 bit word as 8 + 6 bits through two tables
// built at init: 16 table steps a line instead of 112 bit steps, which
// measured 15 us for the whole block on the board. Same result as the
// bitwise form (checked offline against it and against the norm's
// polynomial division).
static uint16_t crcTab8[256];
static uint16_t crcTab6[64];

static void crcTablesInit() {
  for (int i = 0; i < 256; i++) {
    uint16_t c = (uint16_t)(i << 8);
    for (int b = 0; b < 8; b++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1));
    crcTab8[i] = c;
  }
  for (int i = 0; i < 64; i++) {
    uint16_t c = (uint16_t)(i << 10);
    for (int b = 0; b < 6; b++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1));
    crcTab6[i] = c;
  }
}

static inline uint16_t crcWords(const uint16_t *w, int n) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < n; i++) {
    crc = (uint16_t)((crc << 8) ^ crcTab8[((crc >> 8) ^ (w[i] >> 6)) & 0xFF]);
    crc = (uint16_t)((crc << 6) ^ crcTab6[((crc >> 10) ^ w[i]) & 0x3F]);
  }
  return crc;
}

// Active pixel where each of the 137 line bits starts (index 137 = the
// end of the last one), from bit b of 168 starting 36 b / 7 pixels
// after 0H. Computed once: doing the division per bit while drawing
// measured 60 us a line.
static uint16_t bitX[LINE_BITS + 1];
static uint8_t bitWide[LINE_BITS];  // 1 when the bit is 6 pixels wide, else 5

static void bitTableInit() {
  for (int i = 0; i <= LINE_BITS; i++) {
    int x = (36 * (FIRST_BIT + i) + 3) / 7 - ACTIVE_ORIGIN_PX;
    if (x < 0) x = 0;
    if (x > SCREEN_W) x = SCREEN_W;
    bitX[i] = (uint16_t)x;
  }
  for (int i = 0; i < LINE_BITS; i++) bitWide[i] = (bitX[i + 1] - bitX[i]) >= 6 ? 1 : 0;
}

// 50/15 us pre-emphasis (IEC 60841 clause 9.2, fig. 5) as a first order
// shelf on the 44.1 kHz samples, bilinear transform of
// (1 + s t1) / (1 + s t2) at fs = 44100 without pre-warping: DC gain 1,
// +10.46 dB at Nyquist. Against the analogue curve it reads +0.2 dB at
// 5 kHz, +0.7 dB at 10 kHz and +0.9 dB at 20 kHz (checked numerically);
// pre-warping the corners made the top end worse, not better.
// Coefficients Q14: y = (b0 x + b1 x[-1] - a1 y[-1]) / 2^14.
static const int32_t EMPH_B0 = 38156, EMPH_B1 = -24051, EMPH_A1 = -2278;
static int32_t emphXL, emphYL, emphXR, emphYR;

static inline int16_t emphasise(int32_t x, int32_t &x1, int32_t &y1) {
  int64_t acc = (int64_t)EMPH_B0 * x + (int64_t)EMPH_B1 * x1 - (int64_t)EMPH_A1 * y1;
  int32_t y = (int32_t)(acc >> 14);
  if (y > 32767) y = 32767; else if (y < -32768) y = -32768;
  x1 = x;
  y1 = y;
  return (int16_t)y;
}

static void sonyReset() {
  memset(ringL, 0, sizeof(ringL));
  memset(ringR, 0, sizeof(ringR));
  blk = 0;
  pcm1808Flush();   // no stale audio from the last PCM session
  emphXL = emphYL = emphXR = emphYR = 0;
  crcTablesInit();
  bitTableInit();
  asrcReset(44100);
  // The control block and the first data lines sit in the vertical
  // blanking; the encoder only lets lines 7-22 out with the VBI open
  // (register 0x83 bit 4, advSetVbiOpen()). Forced on here rather than
  // warned about as teletext does: without it the card is silent.
  if (!advVbiOpen()) advSetVbiOpen(true);
  // No WSS on line 23: that is a data line here, and the encoder would
  // overwrite it. selectPattern() applies the card's aspect setting
  // (and so the WSS) before init(), so this runs after it; the next
  // card's own aspectSet() puts the WSS back as that card wants it.
  advSetWss(false, 0);
}

static void sonyInit() {
  const int opt = cfgCardOption(-1);
  mode16 = (opt & OPT_16BIT) != 0;
  emphasis = (opt & OPT_EMPHASIS) != 0;
  ctrlInPicture = (opt & OPT_CTRL_IN_PICTURE) != 0;
  const uint32_t *ln = ctrlInPicture ? PIC_LINES : STD_LINES;
  CTRL_LINE_F1 = ln[0]; DATA_FIRST_F1 = ln[1]; DATA_LAST_F1 = ln[2];
  CTRL_LINE_F2 = ln[3]; DATA_FIRST_F2 = ln[4]; DATA_LAST_F2 = ln[5];
  sonyReset();
}

// Writes the 137 line bits (data sync, 8 words, CRC, stop) into the
// active part of a line. Every loop iteration costs a few cycles of
// its own on top of the store, ~9 ns each at 108 MHz (measured: a
// plain 360 word fill was 13 us and byte stores in a loop 42 us), hence
// the unrolling and the bit stream assembled with shifts.
static void drawLine(uint8_t *base, const uint16_t *w, uint16_t crc) {
  uint32_t *p = (uint32_t *)base;
  for (int i = 0; i < SCREEN_W / 2; i += 8) {
    p[0] = CELL_BLACK; p[1] = CELL_BLACK; p[2] = CELL_BLACK; p[3] = CELL_BLACK;
    p[4] = CELL_BLACK; p[5] = CELL_BLACK; p[6] = CELL_BLACK; p[7] = CELL_BLACK;
    p += 8;
  }

  // The 137 bits MSB first in five words: "1010", 8 x 14, 16, "0", "1111".
  // Bit i of the stream is bit (31 - i % 32) of s[i / 32].
  uint32_t s[5];
  s[0] = (0xAu << 28) | ((uint32_t)w[0] << 14) | w[1];
  s[1] = ((uint32_t)w[2] << 18) | ((uint32_t)w[3] << 4) | (w[4] >> 10);
  s[2] = ((uint32_t)(w[4] & 0x3FF) << 22) | ((uint32_t)w[5] << 8) | (w[6] >> 6);
  s[3] = ((uint32_t)(w[6] & 0x3F) << 26) | ((uint32_t)w[7] << 12) | (crc >> 4);
  s[4] = ((uint32_t)(crc & 0xF) << 28) | (0xFu << 23);  // "0" then "1111"

  // Only the ones: clz jumps straight to the next set bit.
  for (int k = 0; k < 5; k++) {
    uint32_t v = s[k];
    while (v) {
      const int lz = __builtin_clz(v);
      v &= ~(0x80000000u >> lz);
      const int i = k * 32 + lz;   // never past LINE_BITS: s[4] has zeros there
      uint8_t *y = base + 2 * bitX[i] + 1;
      y[0] = Y_WHITE; y[2] = Y_WHITE; y[4] = Y_WHITE; y[6] = Y_WHITE; y[8] = Y_WHITE;
      if (bitWide[i]) y[10] = Y_WHITE;
    }
  }
}

static void dataLine(uint8_t *base) {
  int16_t l[3], r[3];
  asrcPull(l, r, 3);
  if (emphasis) {
    for (int i = 0; i < 3; i++) {
      l[i] = emphasise(l[i], emphXL, emphYL);
      r[i] = emphasise(r[i], emphXR, emphYR);
    }
  }
  const int k = blk;
  int16_t *L = ringL[k % DEPTH];
  int16_t *R = ringR[k % DEPTH];
  for (int i = 0; i < 3; i++) { L[i] = l[i]; R[i] = r[i]; }
  blk = (blk + 1 == DEPTH) ? 0 : blk + 1;

  // The block this line's P belongs to, and the one for Q/S.
  const int16_t *Lp = ringL[(k + DEPTH - 96) % DEPTH];
  const int16_t *Rp = ringR[(k + DEPTH - 96) % DEPTH];
  const int16_t *Lq = ringL[(k + DEPTH - 112) % DEPTH];
  const int16_t *Rq = ringR[(k + DEPTH - 112) % DEPTH];

  uint16_t w[8];
  w[0] = q14(ringL[k % DEPTH][0]);
  w[1] = q14(ringR[(k + DEPTH - 16) % DEPTH][0]);
  w[2] = q14(ringL[(k + DEPTH - 32) % DEPTH][1]);
  w[3] = q14(ringR[(k + DEPTH - 48) % DEPTH][1]);
  w[4] = q14(ringL[(k + DEPTH - 64) % DEPTH][2]);
  w[5] = q14(ringR[(k + DEPTH - 80) % DEPTH][2]);
  if (!mode16) {
    w[6] = q14(Lp[0]) ^ q14(Rp[0]) ^ q14(Lp[1]) ^ q14(Rp[1]) ^ q14(Lp[2]) ^ q14(Rp[2]);
    // Horner form of T^6 L0 ^ T^5 R0 ^ T^4 L1 ^ T^3 R1 ^ T^2 L2 ^ T R2.
    uint16_t q = matT(q14(Lq[0]));
    q = matT(q ^ q14(Rq[0]));
    q = matT(q ^ q14(Lq[1]));
    q = matT(q ^ q14(Rq[1]));
    q = matT(q ^ q14(Lq[2]));
    q = matT(q ^ q14(Rq[2]));
    w[7] = q;
  } else {
    uint16_t p16 = (uint16_t)Lp[0] ^ (uint16_t)Rp[0] ^ (uint16_t)Lp[1] ^ (uint16_t)Rp[1] ^
                   (uint16_t)Lp[2] ^ (uint16_t)Rp[2];
    w[6] = (uint16_t)((p16 >> 2) & 0x3FFF);
    uint16_t ps = (uint16_t)Lq[0] ^ (uint16_t)Rq[0] ^ (uint16_t)Lq[1] ^ (uint16_t)Rq[1] ^
                  (uint16_t)Lq[2] ^ (uint16_t)Rq[2];
    w[7] = (uint16_t)(((Lq[0] & 3) << 12) | ((Rq[0] & 3) << 10) | ((Lq[1] & 3) << 8) |
                      ((Rq[1] & 3) << 6) | ((Lq[2] & 3) << 4) | ((Rq[2] & 3) << 2) | (ps & 3));
  }
  drawLine(base, w, crcWords(w, 8));
}

static void controlLine(uint8_t *base) {
  // Control word, bit 1 = MSB: bit 13 (0x0002) set = Q not applied, the
  // 16 bit variant; bit 14 (0x0001) set = no pre-emphasis.
  uint16_t ct = 0;
  if (mode16) ct |= 0x0002;
  if (!emphasis) ct |= 0x0001;
  const uint16_t w[8] = {0x3333, 0x0CCC, 0x3333, 0x0CCC, 0x0000, 0x0000, 0x0000, ct};
  drawLine(base, w, crcWords(w, 8));
}

// Everything by absolute line number, for both hooks.
static bool renderLine(uint8_t *base, uint32_t line) {
  pcm1808Poll();
  if (ctrlInPicture && line <= 6) line += 625;   // field 2's tail, see PIC_LINES
  if (line == CTRL_LINE_F1 || line == CTRL_LINE_F2) {
    if (line == CTRL_LINE_F1) asrcSteer();
    controlLine(base);
    return true;
  }
  if ((line >= DATA_FIRST_F1 && line <= DATA_LAST_F1) ||
      (line >= DATA_FIRST_F2 && line <= DATA_LAST_F2)) {
    dataLine(base);
    return true;
  }
  return false;
}

static void sonyRenderRow(uint8_t *base, int y) {
  const uint32_t line = (y & 1) ? 336 + (uint32_t)(y >> 1) : 23 + (uint32_t)(y >> 1);
  if (!renderLine(base, line)) {
    uint32_t *p = (uint32_t *)base;
    for (int i = 0; i < SCREEN_W / 2; i++) *p++ = CELL_BLACK;
  }
}

static bool sonyVbiLine(uint8_t *base, uint32_t line) {
  return renderLine(base, line);
}

// mono: luma only, and the encoder's burst off with it. keepEdges: the
// data sync starts at pixel 1.7. rawSignal: no ticker, bars, WSS,
// teletext or ITS, the format owns every line.
const Pattern PATTERN_SONYPCM = {LANG_CARD_SONY_PCM, sonyInit, sonyRenderRow, nullptr,
                                 true, false, nullptr, sonyVbiLine, true, true};

// PCM audio experiment card: a PCM1808 on GP9-11/GP13 (see board.h,
// src/pcm1808.h) feeds 48 kHz/16 bit stereo pairs into every active
// row as an NRZ bit stream, so the "picture" carries the sound rather
// than a separate channel -- the idea behind the Sony PCM-501ES, not a
// literal copy of its format. Decoded on a PC by PcmDecoder (PC
// Software/pcmdecoder) from a Blackmagic capture of the composite
// output; measured end to end at 48000 pairs/s with a 1 kHz tone clean.
//
// Its own digital scheme, not a modulation standard to match: NRZ
// bits, self-describing with a header byte rather than a fixed pair
// count, since there is no existing decoder to be compatible with.
// Every row is independent -- whatever pcm1808TakePairs() has queued
// goes out, nothing is padded or waited for, and nothing is spread
// over other rows: latency is one row.
//
// Line code v3, for an FM amateur-television link (sparkles: short
// hits of a few bits; thermal noise) with the least latency: 180 bits
// a row in the middle 696 of the 720 active pixels (GUARD_PX below on
// the 12 pixel guards), cells of 696/180 = 3.87 px:
//   8 run-in, 4 guard, 8 sync byte,
//   core codeword, RS(13,9): header (0xC0 | pairs, 0..4), 8 core bytes,
//     4 parity;
//   enhancement: 6 bytes, CRC-8.
// Each 14 bit sample (two's complement) is split into a core byte, its
// top 8 bits, and a 6 bit enhancement; 4 pairs give 8 core bytes and
// 48 enhancement bits. The RS code (GF(256), x^8+x^4+x^3+x^2+1, g(x) =
// (x-1)(x-a)(x-a^2)(x-a^3)) corrects two bytes anywhere in the core
// codeword, header included, or up to four the decoder knows to be
// doubtful; the enhancement is only checked (CRC-8, 0x07, init 0), a
// decoder that finds it damaged plays the row at 8 bits rather than
// leaving a hole, and one that loses the core codeword conceals the
// row. Core and enhancement bytes are XORed with a fixed keystream
// (16 bit LFSR x^16+x^14+x^13+x^11+1, seed 0xACE1) before parity and
// CRC, so silence is not a long flat run for the receiver's clamp.
// The run-in is 8 bits, not 16: the bit period is fixed by the raster
// and a decoder measured it at 4.00 every second; its polarity flips
// every other row of a field, so the left edge is not a set of still
// lines. The header's three spare bits (5..3) are a text channel: bit
// 5 marks the first of TEXT_SLOTS rows, bits 4..3 carry two bits of
// the PCM text, TEXT_CHARS characters of 6 bits from
// TEXT_ALPHABET (cfgPcmText(), its own setting), so the text repeats
// every 30 rows and rides inside the RS codeword.
// Three qualities on one card, see MODE_HQ below. Reference model with every constant:
// tools/pcm_v3_ref.py, which also writes pcm_v3_tables.h. 14 bit
// samples rather than 16 pay for the parity: 84 dB of range, more
// than the link has.
//
// The card's first and second line sit side by side in a black band
// across the middle, carrying no audio. Budget for that: the ADC
// delivers 3.07 pairs a line (48000 / 15625) and a row takes at most
// MAX_PAIRS, so the ring only drains at 0.93 a row; a field's 2.7 ms
// blanking already parks ~130 pairs, and every text row parks another
// 3.07. Up to ~34 rows a field still drain before the next blanking;
// the band is 32 frame rows, 16 a field, placed mid-picture where the
// blanking backlog has already gone. See pcm1808.cpp for the ring size.
#include <string.h>

#include <pico.h>   // __not_in_flash_func

#include "../adv7391.h"
#include "../gfx.h"
#include "../language.h"
#include "../pattern.h"
#include "../asrc.h"
#include "../pcm1808.h"
#include "../pcm_v3_tables.h"
#include "../settings.h"
#include "../textoverlay.h"

// Cells of roughly 4 pixels: a PcmDecoder capture on a real Blackmagic
// card found the composite round trip does not just smear edges but
// periodically inserts an extra, unplanned 2 pixel pair -- roughly
// once every 15-16 bits at 2 px/bit, matching a ~6.5% mismatch between
// the encoder's own clock and the capture card's recovered one. At
// 2 px/bit that is a full bit's worth of misalignment every time,
// always fatal; near 4 px/bit the same 2 pixel slip is only half a
// cell, which a decoder resyncing every byte can tolerate.
//
// GUARD_PX: no data in the first and last 12 pixels of the line.
// Every 720x576 sampler cuts its own window: this chain's Blackmagic
// puts the Sony card's data sync on pixel 0.2 where 7.7 was
// calculated, so about 7.5 pixels fall off the end of the line, and
// the CRC that used to end the row at pixel 719 arrived at the
// decision threshold (PcmDecoder grew RepairTail for exactly that).
// 12 pixels either side cover that shift both ways with margin; the
// cost is the cells shrinking from 720/N to 696/N.
static const int GUARD_PX = 12;
static const int DATA_PX = 720 - 2 * GUARD_PX;
static const uint8_t SYNC_BYTE = 0x2E;

// Packed GFX_BLACK (Cb=0x80, Y=0x10, Cr=0x80, Y), matching how
// crosshatch.cpp fills its own solid rows; a 1 bit is painted as luma
// 0xEB stores through the cell table.
static const uint32_t CELL_BLACK = 0x10801080u;

// 4 pairs a row, 180 bits x 4 px = 720 pixels exactly (see the header
// comment). 574 rows x 25 Hz x 4 = 57400 pairs/s against the 48000 the
// ADC delivers.
static const int MAX_PAIRS = 4;
static const uint8_t HEADER_V3 = 0xC0;
static const int RUN_IN_BITS = 8;
static const int CORE_BYTES = 8;   // 8 samples, top 8 bits each
static const int ENH_BYTES = 6;    // 8 samples, low 6 bits each

// Quality, the card's option bits 1..0 (cfgCardOption(), the PC tool's
// drop-down and the menu): HQ is the row above; the two others run at
// 32 kHz (the ASRC's exact 3:2 from the ADC's 48 kHz, never steered,
// a row taking what the converter can cover, asrcAvailable()) with 3
// pairs a row at most, the same RS(11,7) core codeword (header, 6 core
// bytes, 4 parity) and the same text channel:
//   LQ    12 bit samples: core byte = bits 11..4, a 4 bit enhancement
//         each (24 bits, 3 bytes, scrambled with key bytes 8..10) and
//         its CRC-8: 20 + 88 + 24 + 8 = 140 bits, cells of 696/140 =
//         4.97 px, 2.72 Mbit/s.
//   Voice 8 bit A-law (ITU-T G.711, the even bits inverted) in the core
//         byte, nothing else: 20 + 88 = 108 bits, cells of 6.44 px,
//         2.09 Mbit/s, every bit inside the RS codeword.
//   Voice narrow: mono, 16 kHz (the ASRC's exact 3:1), the two channels
//         averaged, 2 samples a row in a RS(7,3) codeword (header, 2
//         A-law bytes, 4 parity): 20 + 56 = 76 bits, cells of 9.16 px,
//         1.47 Mbit/s.
// Header bits 7..6 name the row: 11 HQ, 10 LQ, 01 Voice, 00 Voice
// narrow; a decoder tells them apart by the bit period first.
enum { MODE_HQ = 0, MODE_LQ = 1, MODE_VOICE = 2, MODE_NARROW = 3 };
static int mode;
static const int LOW_PAIRS = 3;
static const uint8_t HEADER_LQ = 0x80, HEADER_VOICE = 0x40, HEADER_NARROW = 0x00;
static const int LOW_CORE_BYTES = 6;
static const int LQ_ENH_BYTES = 3;
static const int LQ_BITS = RUN_IN_BITS + 4 + 8 + 8 * (1 + LOW_CORE_BYTES + 4) + 8 * LQ_ENH_BYTES + 8;
static const int VOICE_BITS = RUN_IN_BITS + 4 + 8 + 8 * (1 + LOW_CORE_BYTES + 4);
static const int NARROW_SAMPLES = 2;
static const int NARROW_BITS = RUN_IN_BITS + 4 + 8 + 8 * (1 + NARROW_SAMPLES + 4);
static const int HQ_BITS = RUN_IN_BITS + 4 + 8 + 8 * (1 + CORE_BYTES + 4) + 8 * ENH_BYTES + 8;
static_assert(HQ_BITS == 180 && LQ_BITS == 140 && VOICE_BITS == 108 && NARROW_BITS == 76,
              "the rows are 180, 140, 108 and 76 bits");
static int rowBits;                    // HQ_BITS .. NARROW_BITS
static uint16_t cellX[HQ_BITS + 1];    // first pixel of each bit cell
static uint8_t cellW[HQ_BITS];         // its width in pixels, 3..10

static void cellTableInit(int bits) {
  rowBits = bits;
  for (int i = 0; i <= bits; i++) cellX[i] = (uint16_t)(GUARD_PX + (i * DATA_PX + bits / 2) / bits);
  for (int i = 0; i < bits; i++) cellW[i] = (uint8_t)(cellX[i + 1] - cellX[i]);
}

// G.711 A-law of a 16 bit sample (the top 13 bits are used), even bits
// inverted, as every A-law codec does it.
static uint8_t __not_in_flash_func(alawEncode)(int16_t pcm) {
  int v = pcm >> 3;
  uint8_t mask = 0xD5;
  if (v < 0) { mask = 0x55; v = -v - 1; }
  int seg = 0;
  while (seg < 8 && v > ((0x20 << seg) - 1)) seg++;
  if (seg >= 8) return (uint8_t)(0x7F ^ mask);
  int aval = seg << 4;
  aval |= (seg < 2) ? ((v >> 1) & 0xF) : ((v >> seg) & 0xF);
  return (uint8_t)(aval ^ mask);
}
static const int TEXT_CHARS = 10;
static const int TEXT_SLOTS = TEXT_CHARS * 6 / 2;   // two bits a row
static const char TEXT_ALPHABET[65] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,-/:;!?'()+=&@#*%<>\"_[]{}|";
static uint8_t textBits[TEXT_SLOTS];   // the 2 bit values, header bits 4..3
static uint8_t textSlot;

// The PCM text as TEXT_CHARS symbols of the alphabet: upper case,
// anything outside it a space, padded with spaces.
static void textChannelPrepare(const char *s) {
  uint8_t sym[TEXT_CHARS];
  for (int i = 0; i < TEXT_CHARS; i++) {
    char c = *s ? *s++ : ' ';
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    const char *hit = (c != 0) ? strchr(TEXT_ALPHABET, c) : nullptr;
    sym[i] = hit ? (uint8_t)(hit - TEXT_ALPHABET) : 0;
  }
  for (int k = 0; k < TEXT_SLOTS; k++) {
    const int bit = 2 * k;   // MSB first through the 60 bit string
    textBits[k] = (uint8_t)((sym[bit / 6] >> (4 - (bit % 6))) & 3);
  }
}
// Systematic RS parity of the 9 byte core codeword: the remainder of
// data(x) x^4 divided by g(x), one table lookup a byte (RS_STEP packs
// the four products).
static inline uint32_t rsParity(const uint8_t *data, int n) {
  uint32_t rem = 0;
  for (int i = 0; i < n; i++) rem = (rem << 8) ^ RS_STEP[(uint8_t)((rem >> 24) ^ data[i])];
  return rem;
}

static inline uint8_t crc8(const uint8_t *d, int n) {
  uint8_t c = 0;
  for (int i = 0; i < n; i++) c = CRC8_TABLE[c ^ d[i]];
  return c;
}

// A PcmDecoder capture found a wide, unbroken high stretch riding over
// the run-in-to-sync-byte boundary, well past the one bit width that
// belongs there, at both 2 and 4 px/bit -- a filter settling from the
// run-in's constant toggling into the sync byte's different pattern,
// not a timing error. These bits, always low, give it a quiet stretch
// to settle in before the framing byte a decoder actually needs to
// read starts.
static const int GUARD_BITS = 4;

// The text band: one line height, the first line in the left half and
// the second in the right (two lines stacked sat too close for the
// eye); with only one of them on, that one centred over the full
// width. TEXT_MARGIN keeps the text off the picture edge.
static const int TEXT_Y0 = 260;
static const int TEXT_BAND_H = 32;
static const int TEXT_Y1 = TEXT_Y0 + TEXT_BAND_H;
static const int TEXT_MARGIN = 16;

static TextOverlay lineId, lineSub;
static bool showId, showSub;

// The light half of the init: only the text layout. See pattern.h.
// Both lines are asked for unconditionally, switch on or off: that is
// what tells cfgSetActiveCard() this card has the slots at all.
static void pcmAudioReflow() {
  const char *id = cfgTextIdShown();
  textChannelPrepare(cfgPcmText());
  const char *sub = cfgTextSubShown();
  showId = cfgShowId();
  showSub = cfgShowSub();
  const bool both = showId && showSub;
  const int mid = SCREEN_W / 2;
  if (showId) {
    textPrepare(&lineId, id, TEXT_MARGIN, both ? mid - TEXT_MARGIN / 2 : SCREEN_W - TEXT_MARGIN,
                TEXT_Y0, TEXT_Y1);
  }
  if (showSub) {
    textPrepare(&lineSub, sub, both ? mid + TEXT_MARGIN / 2 : TEXT_MARGIN, SCREEN_W - TEXT_MARGIN,
                TEXT_Y0, TEXT_Y1);
  }
}

static void pcmAudioInit() {
  mode = cfgCardOption(-1) & 3;
  if (mode == MODE_NARROW) {
    cellTableInit(NARROW_BITS);
    asrcReset(16000);
  } else if (mode != MODE_HQ) {
    cellTableInit(mode == MODE_VOICE ? VOICE_BITS : LQ_BITS);
    asrcReset(32000);
  } else {
    cellTableInit(HQ_BITS);
  }
  pcm1808Flush();   // no stale audio from the last PCM session
  pcmAudioReflow();
  // No WSS on line 23 and no ticker (rawSignal): the row code owns
  // every active line. selectPattern() applies the card's aspect
  // setting (and so the WSS) before init(), so this runs after it; the
  // next card's own aspectSet() puts the WSS back as that card wants it.
  advSetWss(false, 0);
}

// The LQ and Voice rows: the bits packed MSB first into five words, the
// row cleared black in words, then only the 1 bits painted, cellW luma
// bytes each through the cell table, found with clz (the Sony card's
// method; a loop over a bit array cost 100 us a row and starved the
// streamer).
// In RAM and never inlined: as one function with the HQ row in flash,
// the 32 kHz rows cost 72 us against the 55 the same code took as a
// card of its own, the flash cache again (see videoBuildLine()).
// Paints the 1 bits of st[] (MSB first, rowBits of them, up to 180 in
// six words) through the cell table: three stores straight, the rest by
// the cell's width (3 or 4 px for HQ, 4 or 5 for LQ, 6 or 7 for Voice,
// 9 or 10 for Voice narrow). A loop over the width cost 69 us a row
// against unrolled stores (measured, at the older 5..7 px cells).
// One shared copy in RAM, not inline: inlined into its three RAM
// callers it overflowed the 512 kB by 516 bytes.
static void __attribute__((noinline)) paintCells(uint8_t *base, const uint32_t *st);
static void __not_in_flash_func(paintCells)(uint8_t *base, const uint32_t *st) {
  uint32_t *w = (uint32_t *)base;
  for (int i = 0; i < SCREEN_W / 2; i++) w[i] = CELL_BLACK;
  for (int k = 0; k < 6; k++) {
    uint32_t v = st[k];
    while (v) {
      const int lz = __builtin_clz(v);
      v &= ~(0x80000000u >> lz);
      const int i = k * 32 + lz;
      if (i >= rowBits) break;
      uint8_t *py = base + 2 * cellX[i] + 1;
      const int cw = cellW[i];
      py[0] = 0xEB; py[2] = 0xEB; py[4] = 0xEB;
      if (cw > 3) py[6] = 0xEB;
      if (cw > 4) py[8] = 0xEB;
      if (cw > 5) py[10] = 0xEB;
      if (cw > 6) py[12] = 0xEB;
      if (cw > 7) { py[14] = 0xEB; py[16] = 0xEB; }
      if (cw > 9) py[18] = 0xEB;
    }
  }
}

// Voice narrow: 2 mono samples a row, the pair averaged, A-law, in a
// RS(7,3) codeword; 76 bits.
static void __attribute__((noinline)) narrowRenderRow(uint8_t *base, int y);
static void __not_in_flash_func(narrowRenderRow)(uint8_t *base, int y) {
  int16_t l[NARROW_SAMPLES], r[NARROW_SAMPLES];
  int n = 0;
  if (y >= 2) {
    n = asrcAvailable();
    if (n > NARROW_SAMPLES) n = NARROW_SAMPLES;
    if (n > 0) asrcPull(l, r, n);
  }
  uint8_t cw[1 + NARROW_SAMPLES];
  cw[0] = (uint8_t)(HEADER_NARROW | n | (textSlot == 0 ? 0x20 : 0) | (textBits[textSlot] << 3));
  if (++textSlot >= TEXT_SLOTS) textSlot = 0;
  for (int i = 0; i < NARROW_SAMPLES; i++) {
    const int16_t m = i < n ? (int16_t)(((int32_t)l[i] + r[i]) >> 1) : 0;
    cw[1 + i] = alawEncode(m) ^ SCRAMBLE_KEY[i];
  }
  const uint32_t par = rsParity(cw, 1 + NARROW_SAMPLES);
  const int pol = (y >> 1) & 1;
  uint32_t st[6];
  st[0] = ((pol ? 0xAAu : 0x55u) << 24) | ((uint32_t)SYNC_BYTE << 12) | ((uint32_t)cw[0] << 4) | (cw[1] >> 4);
  st[1] = ((uint32_t)cw[1] << 28) | ((uint32_t)cw[2] << 20) | (par >> 12);
  st[2] = par << 20;
  st[3] = 0;
  st[4] = 0;
  st[5] = 0;
  paintCells(base, st);
}

static void __attribute__((noinline)) lowRateRenderRow(uint8_t *base, int y);
static void __not_in_flash_func(lowRateRenderRow)(uint8_t *base, int y) {
  int16_t l[LOW_PAIRS], r[LOW_PAIRS];
  int n = 0;
  if (y >= 2) {
    n = asrcAvailable();
    if (n > LOW_PAIRS) n = LOW_PAIRS;
    if (n > 0) asrcPull(l, r, n);
  }
  const bool voice = mode == MODE_VOICE;
  uint8_t cw[1 + LOW_CORE_BYTES];
  uint8_t enh[LQ_ENH_BYTES] = {0, 0, 0};
  uint8_t crc = 0;
  cw[0] = (uint8_t)((voice ? HEADER_VOICE : HEADER_LQ) | n | (textSlot == 0 ? 0x20 : 0) | (textBits[textSlot] << 3));
  if (++textSlot >= TEXT_SLOTS) textSlot = 0;
  if (voice) {
    for (int i = 0; i < LOW_PAIRS; i++) {
      cw[1 + 2 * i] = alawEncode(i < n ? l[i] : 0) ^ SCRAMBLE_KEY[2 * i];
      cw[2 + 2 * i] = alawEncode(i < n ? r[i] : 0) ^ SCRAMBLE_KEY[2 * i + 1];
    }
  } else {
    uint8_t v[6];   // the six 4 bit enhancements, L0 R0 L1 R1 L2 R2
    for (int i = 0; i < LOW_PAIRS; i++) {
      const uint32_t lw = i < n ? (uint32_t)((l[i] >> 4) & 0xFFF) : 0;
      const uint32_t rw = i < n ? (uint32_t)((r[i] >> 4) & 0xFFF) : 0;
      cw[1 + 2 * i] = (uint8_t)(lw >> 4) ^ SCRAMBLE_KEY[2 * i];
      cw[2 + 2 * i] = (uint8_t)(rw >> 4) ^ SCRAMBLE_KEY[2 * i + 1];
      v[2 * i] = (uint8_t)(lw & 0xF);
      v[2 * i + 1] = (uint8_t)(rw & 0xF);
    }
    enh[0] = (uint8_t)((v[0] << 4) | v[1]) ^ SCRAMBLE_KEY[CORE_BYTES + 0];
    enh[1] = (uint8_t)((v[2] << 4) | v[3]) ^ SCRAMBLE_KEY[CORE_BYTES + 1];
    enh[2] = (uint8_t)((v[4] << 4) | v[5]) ^ SCRAMBLE_KEY[CORE_BYTES + 2];
    crc = crc8(enh, LQ_ENH_BYTES);
  }
  const uint32_t par = rsParity(cw, 1 + LOW_CORE_BYTES);

  // The bit stream, MSB first: run-in (8), guard (4), sync (8), the 7
  // codeword bytes, 4 parity bytes, then for LQ the 3 enhancement bytes
  // and the CRC: 140 bits in st[0..4], 108 for Voice (st[4] empty).
  const int pol = (y >> 1) & 1;
  uint32_t st[6];
  st[0] = ((pol ? 0xAAu : 0x55u) << 24) | ((uint32_t)SYNC_BYTE << 12) | ((uint32_t)cw[0] << 4) | (cw[1] >> 4);
  st[1] = ((uint32_t)cw[1] << 28) | ((uint32_t)cw[2] << 20) | ((uint32_t)cw[3] << 12) | ((uint32_t)cw[4] << 4) | (cw[5] >> 4);
  st[2] = ((uint32_t)cw[5] << 28) | ((uint32_t)cw[6] << 20) | (par >> 12);
  st[3] = (par << 20) | ((uint32_t)enh[0] << 12) | ((uint32_t)enh[1] << 4) | (enh[2] >> 4);
  st[4] = ((uint32_t)enh[2] << 28) | ((uint32_t)crc << 20);
  st[5] = 0;

  paintCells(base, st);
}

// Blanking lines: nothing to draw, but the ADC ring wants a poll every
// 64 us (pcm1808.cpp on why).
static bool pcmAudioVbiLine(uint8_t *base, uint32_t line) {
  (void)base;
  (void)line;
  pcm1808Poll();
  return false;
}

static void __attribute__((noinline)) hqRenderRow(uint8_t *base, int y);

static void pcmAudioRenderRow(uint8_t *base, int y) {
  // Every row, including the ones that carry no audio: the raw DMA
  // ring only holds 5.3 ms.
  pcm1808Poll();

  if ((showId || showSub) && y >= TEXT_Y0 && y < TEXT_Y1) {
    uint32_t *w = (uint32_t *)base;
    for (int i = 0; i < SCREEN_W / 2; i++) *w++ = CELL_BLACK;
    if (showId) textDrawRow(base, &lineId, y, textInkId());
    if (showSub) textDrawRow(base, &lineSub, y, textInkSub());
    return;
  }

  if (mode == MODE_NARROW) { narrowRenderRow(base, y); return; }
  if (mode != MODE_HQ) { lowRateRenderRow(base, y); return; }
  hqRenderRow(base, y);
}

// The HQ row: 4 pairs from the ADC ring, packed into six words and
// painted through the cell table like the other modes. In flash, not
// RAM: the old HQ path also ran from flash at a measured 40 us, the
// painting itself (paintCells) is in RAM, and one more RAM function
// overflowed the 512 kB. Compare [timing] after any build that moves
// code around (the 16 kB XIP cache, see videoBuildLine()).
static void hqRenderRow(uint8_t *base, int y) {
  int16_t l[MAX_PAIRS], r[MAX_PAIRS];
  // The first row of each field (0 and 1) never reaches the capture
  // card: a Blackmagic Intensity Pro in PAL mode delivers its 576 rows
  // one line later than this raster draws them, so its row 0 is this
  // card's row 2, its last two rows show lines that do not exist here
  // (they failed the run-in every frame, and no others did), and
  // whatever is put in rows 0 and 1 is lost -- measured as exactly
  // 8 pairs a frame short on the PC, heard as a 25 Hz tick on a pure
  // tone. Left empty.
  int n = (y < 2) ? 0 : pcm1808TakePairs(l, r, MAX_PAIRS);

  // Core codeword: header, the 8 core bytes (scrambled), 4 parity;
  // then the 48 enhancement bits (scrambled) and their CRC.
  uint8_t cw[1 + CORE_BYTES];
  uint8_t enh[ENH_BYTES];
  cw[0] = (uint8_t)(HEADER_V3 | n | (textSlot == 0 ? 0x20 : 0) | (textBits[textSlot] << 3));
  if (++textSlot >= TEXT_SLOTS) textSlot = 0;
  {
    uint32_t acc = 0;
    int nb = 0, o = 0;
    for (int i = 0; i < MAX_PAIRS; i++) {
      const uint32_t lw = i < n ? (uint32_t)((l[i] >> 2) & 0x3FFF) : 0;
      const uint32_t rw = i < n ? (uint32_t)((r[i] >> 2) & 0x3FFF) : 0;
      cw[1 + 2 * i] = (uint8_t)(lw >> 6) ^ SCRAMBLE_KEY[2 * i];
      cw[2 + 2 * i] = (uint8_t)(rw >> 6) ^ SCRAMBLE_KEY[2 * i + 1];
      acc = (acc << 6) | (lw & 0x3F); nb += 6;
      acc = (acc << 6) | (rw & 0x3F); nb += 6;
      while (nb >= 8) { enh[o] = (uint8_t)(acc >> (nb - 8)) ^ SCRAMBLE_KEY[CORE_BYTES + o]; o++; nb -= 8; }
    }
  }
  const uint32_t par = rsParity(cw, 1 + CORE_BYTES);
  const uint8_t crc = crc8(enh, ENH_BYTES);

  // The bit stream, MSB first: run-in (8, the clock run-in with the
  // same purpose as teletext's own, ETS 6.1, its polarity by row),
  // guard (4), sync (8), the 9 codeword bytes, 4 parity, 6 enhancement
  // bytes and the CRC: 180 bits in st[0..5].
  const int pol = (y >> 1) & 1;   // every other row of a field
  uint32_t st[6];
  st[0] = ((pol ? 0xAAu : 0x55u) << 24) | ((uint32_t)SYNC_BYTE << 12) | ((uint32_t)cw[0] << 4) | (cw[1] >> 4);
  st[1] = ((uint32_t)cw[1] << 28) | ((uint32_t)cw[2] << 20) | ((uint32_t)cw[3] << 12) | ((uint32_t)cw[4] << 4) | (cw[5] >> 4);
  st[2] = ((uint32_t)cw[5] << 28) | ((uint32_t)cw[6] << 20) | ((uint32_t)cw[7] << 12) | ((uint32_t)cw[8] << 4) | (par >> 28);
  st[3] = (par << 4) | (enh[0] >> 4);
  st[4] = ((uint32_t)enh[0] << 28) | ((uint32_t)enh[1] << 20) | ((uint32_t)enh[2] << 12) | ((uint32_t)enh[3] << 4) | (enh[4] >> 4);
  st[5] = ((uint32_t)enh[4] << 28) | ((uint32_t)enh[5] << 20) | ((uint32_t)crc << 12);
  paintCells(base, st);
}

// mono = true: pure luma levels, no reason to also burden the encoder
// with a colour subcarrier next to a digital waveform.
const Pattern PATTERN_PCMAUDIO = {LANG_CARD_PCM_AUDIO, pcmAudioInit, pcmAudioRenderRow,
                                  nullptr, true, false, pcmAudioReflow, pcmAudioVbiLine,
                                  true, true};  // keepEdges: the row code paints its own 12 px guards; rawSignal

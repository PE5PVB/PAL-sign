#include "teletext.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef TT_HOSTTEST
// Self test on the PC: see tools/teletext_check.py.
struct ClockTime {
  int year, month, day, hour, min, sec;
};
static long long clockGet(ClockTime *t) {
  t->year = 2026; t->month = 8; t->day = 12;
  t->hour = 12; t->min = 34; t->sec = 56;
  return 0;
}
static const char *cfgTextId() { return "PE5PVB"; }
static const char *cfgTeletextRow(int) { return ""; }   // always the built-in default page
static const int CFG_TELETEXT = 1;
static const int CFG_TELETEXT_PAGE = 0x100;
static bool itsClaimsLine(uint32_t) { return false; }
#define __not_in_flash_func(f) f   // pico.h is not included in this branch
#else
#include "clock.h"
#include <pico.h>   // __not_in_flash_func
#include "config.h"
#include "its.h"
#include "settings.h"
#endif

// From spec to sample positions (ETS 300 706, ITU-R BT.653-3/BT.601-5).
// 13.5 MHz / 6.9375 Mbit/s = 72/37 exactly, so bits do not land on
// samples but the sample/bit
// pattern repeats every 37 bits; the pulse table below leans on that.
static const int TT_SAMPLES = 720;      // luma samples per line (BT.601)
static const int TT_BITS = 360;         // ETS 7.1: 45 bytes = 360 bits
static const int TT_BYTES = 45;

// Centre of bit n, in units of 1/37 of a sample after the first active
// sample.
static const int TT_C0_37 = 246;
static const int TT_STEP_37 = 72;

// The pulse shape: ETS 300 706 5.4 (skew symmetrical, near zero by
// 5 MHz) is a Nyquist spectrum, its usual pulse the raised cosine
// g(x) = sinc(x) . cos(pi.a.x) / (1 - (2ax)^2), normalised to g(0) = 1.
// Roll-off a = 0.60. A Nyquist pulse rings below black between two
// isolated ones; those samples clip at code 1, reported by
// teletextStatus().
static const double TT_ALPHA = 0.60;

// +/-12 samples leaves the omitted tail below 0.001 of the envelope.
static const int TT_TAPS_HALF = 12;
static const int TT_TAPS = 2 * TT_TAPS_HALF + 1;
static const int TT_PHASES = 37;      // see 72/37 above
static const int TT_SCALE = 4096;   // fixed point: 4096 = amplitude 1.0

static int16_t tapTable[TT_PHASES][TT_TAPS];

// BT.601-5: black 16, peak white 235. ETS 5.2: 1 level at 66% of the
// difference, so code 161 (0.21 points off the 66%, within the
// standard's 6 point allowance).
static const int TT_BLACK = 16;
static const int TT_AMPL = 145;  // 161 - 16

static double sinc(double x) {
  if (fabs(x) < 1e-12) return 1.0;
  return sin(M_PI * x) / (M_PI * x);
}

static double pulsvorm(double x) {
  double noemer = 1.0 - (2.0 * TT_ALPHA * x) * (2.0 * TT_ALPHA * x);
  if (fabs(noemer) < 1e-9) {
    // x = +/-1/(2a): 0/0. The limit is (pi/4).sinc(1/(2a)).
    return (M_PI / 4.0) * sinc(x);
  }
  return sinc(x) * cos(M_PI * TT_ALPHA * x) / noemer;
}

static void buildPulseTable() {
  // Phase p: the bit centre lies p/37 of a sample after a sample point.
  for (int p = 0; p < TT_PHASES; p++) {
    for (int j = 0; j < TT_TAPS; j++) {
      double dsample = (double)(j - TT_TAPS_HALF) - (double)p / TT_PHASES;
      double x = dsample * (double)TT_PHASES / (double)TT_STEP_37;
      double v = pulsvorm(x) * TT_SCALE;
      tapTable[p][j] = (int16_t)(v >= 0 ? v + 0.5 : v - 0.5);
    }
  }
}

// Byte coding: ETS 300 706 8.1 (odd parity, bit 8) and 8.2 (Hamming
// 8/4, bytes LSB first).
static uint8_t oneven(uint8_t c) {
  uint8_t d = (uint8_t)(c & 0x7F);
  uint8_t x = d;
  x ^= (uint8_t)(x >> 4);
  x ^= (uint8_t)(x >> 2);
  x ^= (uint8_t)(x >> 1);
  return (uint8_t)((x & 1) ? d : (d | 0x80));
}

static uint8_t hamming84(uint8_t nibble) {
  uint8_t d1 = (uint8_t)(nibble & 1), d2 = (uint8_t)((nibble >> 1) & 1);
  uint8_t d3 = (uint8_t)((nibble >> 2) & 1), d4 = (uint8_t)((nibble >> 3) & 1);
  uint8_t p1 = (uint8_t)(1 ^ d1 ^ d3 ^ d4);
  uint8_t p2 = (uint8_t)(1 ^ d1 ^ d2 ^ d4);
  uint8_t p3 = (uint8_t)(1 ^ d1 ^ d2 ^ d3);
  uint8_t p4 = (uint8_t)(1 ^ p1 ^ d1 ^ p2 ^ d2 ^ p3 ^ d3 ^ d4);
  return (uint8_t)(p1 | (d1 << 1) | (p2 << 2) | (d2 << 3) | (p3 << 4) | (d3 << 5) |
                   (p4 << 6) | (d4 << 7));
}

// The page: hex page number the way teletext does it (0x100 = page 100,
// magazine 1). Characters are 7 bit Latin G0 (ETS table 35); we avoid
// the thirteen positions that differ per national subset, so the
// subset choice does not matter to us. Codes 0x00..0x1F are spacing
// attributes (ETS 12.2): 0x01..0x07 the alpha colours, shown as a space
// and working "Set-After".
static char page[TT_ROWS][TT_COLS];
static char headerRow[TT_HEADER_COLS];

// \0nn are octal colour codes (hex would swallow a following hex digit).
static const char *const DEFAULT_PAGE[TT_ROWS] = {
    "\003ADV7391 TEST CARD GENERATOR",
    "",
    "\007This page comes out of the Pico",
    "\007itself. The ADV7391 has no teletext",
    "\007inserter; the waveform simply sits",
    "\007in the luma samples of the VBI lines.",
    "",
    "\006World System Teletext, 6.9375 Mbit/s",
    "\00613.5 MHz / 6.9375 MHz = 72/37, so",
    "\0061.946 samples per bit. The bits do",
    "\006not land on the samples: the line is",
    "\006worked out as a band limited wave.",
    "",
    "\002Source: ETS 300 706, ITU-R BT.653-3",
    "\002Amplitude 66 %, start 12.0 us after 0H",
};


void teletextSetRow(int row, const char *text) {
  if (row < 1 || row > TT_ROWS) return;
  char *d = page[row - 1];
  int i = 0;
  for (; text && text[i] && i < TT_COLS; i++) d[i] = text[i];
  for (; i < TT_COLS; i++) d[i] = ' ';
}

void teletextApplyRow(int row) {
  if (row < 1 || row > TT_ROWS) return;
  const char *custom = cfgTeletextRow(row);
  teletextSetRow(row, (custom && custom[0]) ? custom : DEFAULT_PAGE[row - 1]);
}

void teletextRowText(int row, char *buf, unsigned n) {
  if (n == 0) return;
  buf[0] = 0;
  if (row < 1 || row > TT_ROWS) return;
  const char *src = page[row - 1];
  // A leading white (7) code is redundant: white is where a row starts
  // without one. Dropped here so this matches the canonical form the PC
  // tool's own encoder produces; otherwise an untouched row that starts
  // this way would read back as "changed" the moment it round-trips.
  int start = (src[0] == 7) ? 1 : 0;
  int len = TT_COLS;
  while (len > start && src[len - 1] == ' ') len--;   // the padding teletextSetRow() adds
  int outLen = len - start;
  if ((unsigned)outLen >= n) outLen = (int)n - 1;
  memcpy(buf, src + start, (size_t)outLen);
  buf[outLen] = 0;
}

// The header: 32 characters (ETS 9.3.1.4, bytes 14..45); the decoder's
// own page-number digits sit to the left of these. The last eight bytes
// carry HH:MM:SS, per the clause's "usually a real-time clock".
static void writeHeader() {
  ClockTime t;
  clockGet(&t);
  const char *name = cfgTextId();
  char buf[TT_HEADER_COLS + 1];
  snprintf(buf, sizeof(buf), "%-7.7s TELETEXT %02d-%02d ", name, t.day, t.month);
  for (int i = 0; i < TT_HEADER_COLS - 8; i++) {
    headerRow[i] = (buf[i] >= 0x20 && buf[i] < 0x7F) ? buf[i] : ' ';
  }
  char clock[9];
  snprintf(clock, sizeof(clock), "%02d:%02d:%02d", t.hour, t.min, t.sec);
  for (int i = 0; i < 8; i++) headerRow[TT_HEADER_COLS - 8 + i] = clock[i];
}

// The packet: ETS 7.1, 45 bytes -- run-in (2), framing code, address
// (2), then 40 data bytes.
static const uint8_t TT_RUNIN = 0x55;
static const uint8_t TT_FRAMING = 0x27;

// Bytes 4 and 5: magazine/row, both Hamming 8/4 (ETS 7.1.2).
static void adres(uint8_t *b, int magazine, int row) {
  b[3] = hamming84((uint8_t)((magazine & 7) | ((row & 1) << 3)));
  b[4] = hamming84((uint8_t)((row >> 1) & 0x0F));
}

static void makePacket(uint8_t *b, int row) {
  memset(b, 0, TT_BYTES);
  b[0] = TT_RUNIN;
  b[1] = TT_RUNIN;
  b[2] = TT_FRAMING;

  int pagnr = CFG_TELETEXT_PAGE;
  int magazine = (pagnr >> 8) & 7;  // 0x8xx -> magazine bits 000
  adres(b, magazine, row);

  if (row == 0) {
    // Header, ETS 9.3.1.
    b[5] = hamming84((uint8_t)(pagnr & 0x0F));         // units
    b[6] = hamming84((uint8_t)((pagnr >> 4) & 0x0F));  // tens
    b[7] = hamming84(0);   // S1
    b[8] = hamming84(0);   // S2 + C4 (erase page)
    b[9] = hamming84(0);   // S3
    b[10] = hamming84(0);  // S4 + C5 (newsflash) + C6 (subtitle)
    b[11] = hamming84(0);  // C7 C8 C9 C10
    // C11 = 1: Serial mode (ETS table 2), a page closes on the next
    // header. C12..C14 (national subset) stay zero, unused by us.
    b[12] = hamming84(0x01);
    for (int i = 0; i < TT_HEADER_COLS; i++) {
      b[13 + i] = oneven((uint8_t)headerRow[i]);
    }
  } else {
    // Row packet, ETS 9.3.2: 40 text bytes with odd parity.
    for (int i = 0; i < TT_COLS; i++) {
      b[5 + i] = oneven((uint8_t)page[row - 1][i]);
    }
  }
}

// Rasterising.
static uint8_t wave[TT_PACKETS][TT_SAMPLES];
static int16_t acc[TT_SAMPLES];  // static: 1440 bytes does not fit on the stack

// How often the undershoot did not fit in 8 bits, and how deep it
// wanted to go; reported so it stays visible. clipLast* is the last row
// rasterised, clipCount/clipDeepest the whole page (teletextInit()); the
// header re-rasterises every second but does not add to those two.
static uint32_t clipLast = 0;
static int clipLastDeepest = 1;
static uint32_t clipCount = 0;
static int clipDeepest = 1;

// Bits go LSB first (ETS 8).
static void rasterPacket(const uint8_t *b, uint8_t *luma) {
  clipLast = 0;
  clipLastDeepest = 1;
  memset(acc, 0, sizeof(acc));
  for (int n = 0; n < TT_BITS; n++) {
    if (!((b[n >> 3] >> (n & 7)) & 1)) continue;  // only ones contribute
    int c37 = TT_C0_37 + TT_STEP_37 * n;
    int basis = c37 / TT_PHASES;
    int phase = c37 % TT_PHASES;
    const int16_t *tap = tapTable[phase];
    int k = basis - TT_TAPS_HALF;
    for (int j = 0; j < TT_TAPS; j++, k++) {
      // Bits near the start stick out to the left of the active window
      // and are dropped; ETS 6.1 allows the run-in's first two ones to
      // be absent or smaller.
      if ((unsigned)k < (unsigned)TT_SAMPLES) acc[k] = (int16_t)(acc[k] + tap[j]);
    }
  }
  for (int k = 0; k < TT_SAMPLES; k++) {
    int v = (int)acc[k] * TT_AMPL;
    int q = (v >= 0) ? (v + TT_SCALE / 2) / TT_SCALE : (v - TT_SCALE / 2) / TT_SCALE;
    int y = TT_BLACK + q;
    // 0x00 and 0xFF are the BT.656 EAV/SAV codes and may never appear.
    if (y < 1) {
      if (y < clipLastDeepest) clipLastDeepest = y;
      clipLast++;
      y = 1;
    }
    if (y > 254) y = 254;
    luma[k] = (uint8_t)y;
  }
}

// Line layout: ETS 4.1 allows 6-22 and 318-335 on a composite signal;
// we use 7-22 and 320-335 (the ADV7391 only passes 7-22 for PAL per its
// datasheet page 47, and says nothing about field 2, hence the same
// page in both fields; see TT_ROWS in teletext.h).
static const uint32_t TT_V1_START = 7;
static const uint32_t TT_V1_END = 22;
static const uint32_t TT_V2_START = 320;
static const uint32_t TT_V2_END = 335;
static const int TT_PER_FIELD = (int)(TT_V1_END - TT_V1_START + 1);
static_assert(TT_PACKETS <= TT_PER_FIELD,
              "the page has to fit in one field, otherwise it depends on field 2");
static_assert(TT_V2_END - TT_V2_START == TT_V1_END - TT_V1_START,
              "both fields have to hold the same number of lines");

static bool on = false;
static int64_t clockState = -1;

void teletextInit() {
  buildPulseTable();
  for (int r = 1; r <= TT_ROWS; r++) teletextApplyRow(r);
  writeHeader();
  clockState = -1;
  clipCount = 0;
  clipDeepest = 1;
  uint8_t b[TT_BYTES];
  for (int p = 0; p < TT_PACKETS; p++) {
    makePacket(b, p);
    rasterPacket(b, wave[p]);
    clipCount += clipLast;
    if (clipLastDeepest < clipDeepest) clipDeepest = clipLastDeepest;
  }
  on = (CFG_TELETEXT != 0);
}

// Rebuilds one row's transmitted waveform after an edit:
// rasterPacket() otherwise runs only at init and, for the header, in
// teletextTick(), so a row edited over the protocol never reached the
// air until the next boot.
void teletextRerasterRow(int row) {
  if (row < 1 || row >= TT_PACKETS) return;
  uint8_t b[TT_BYTES];
  makePacket(b, row);
  rasterPacket(b, wave[row]);
}

bool teletextOn() { return on; }

void teletextSetOn(bool v) { on = v; }

void teletextTick() {
  if (!on) return;
  ClockTime t;
  int64_t nu = clockGet(&t);
  if (nu == clockState) return;
  clockState = nu;
  writeHeader();
  uint8_t b[TT_BYTES];
  makePacket(b, 0);
  rasterPacket(b, wave[0]);  // the header only; the text rows do not change
}

// Which packet belongs on this line. Not simply line minus first line:
// ITS can claim fixed lines out of the range, so teletext gives way and
// its packets close up over the gap, dropping the last two text rows
// rather than opening a hole in the middle of the page.
static int packetForLine(uint32_t line) {
  uint32_t start;
  if (line >= TT_V1_START && line <= TT_V1_END) {
    start = TT_V1_START;
  } else if (line >= TT_V2_START && line <= TT_V2_END) {
    start = TT_V2_START;  // field 2 carries the same page
  } else {
    return -1;
  }
  if (itsClaimsLine(line)) return -1;

  int plaats = 0;
  for (uint32_t l = start; l < line; l++) {
    if (!itsClaimsLine(l)) plaats++;
  }
  return plaats;
}

int teletextPacketsPerField() {
  int n = 0;
  for (uint32_t l = TT_V1_START; l <= TT_V1_END; l++) {
    if (!itsClaimsLine(l)) n++;
  }
  return n < TT_PACKETS ? n : TT_PACKETS;
}

// IN RAM: a photo row's memcpy from flash sweeps the XIP cache, the
// same trap the ticker fell into.
bool __not_in_flash_func(teletextRenderLine)(uint8_t *uyvy, uint32_t line) {
  if (!on) return false;
  int plaats = packetForLine(line);
  if (plaats < 0) return false;
  if (plaats >= TT_PACKETS) return false;

  // 4:2:2 as 32 bit words: U,Y0,V,Y1 little endian, chroma neutral 0x80.
  const uint16_t *pair = (const uint16_t *)wave[plaats];
  uint32_t *w = (uint32_t *)uyvy;
  for (int i = 0; i < TT_SAMPLES / 2; i++) {
    const uint32_t two = pair[i];
    w[i] = 0x00800080u | ((two & 0x00FFu) << 8) | ((two & 0xFF00u) << 16);
  }
  return true;
}

void teletextStatus(char *buf, unsigned n) {
  snprintf(buf, n,
           "%s -- page %03X, %d packets on lines %d-%d and again on %d-%d; "
           "%u bytes RAM; %lu of %d samples clipped at code 1 (deepest wanted %d)",
           on ? "ON" : "off", CFG_TELETEXT_PAGE, TT_PACKETS, (int)TT_V1_START,
           (int)TT_V1_START + TT_PACKETS - 1, (int)TT_V2_START,
           (int)TT_V2_START + TT_PACKETS - 1,
           (unsigned)(sizeof(wave) + sizeof(tapTable) + sizeof(page) + sizeof(acc)),
           (unsigned long)clipCount, TT_PACKETS * TT_SAMPLES, clipDeepest);
}

// For tools/teletext_check.py: builds the same packets and prints the
// bytes/samples, so exactly this code is checked, not a second
// implementation.
#ifdef TT_HOSTTEST
int main() {
  teletextInit();
  printf("# teletext self test output\n");
  printf("alpha %.9f\n", TT_ALPHA);
  printf("taps %d phases %d scale %d\n", TT_TAPS, TT_PHASES, TT_SCALE);
  printf("black %d ampl %d samples %d bits %d\n", TT_BLACK, TT_AMPL, TT_SAMPLES, TT_BITS);
  printf("c0_37 %d step_37 %d\n", TT_C0_37, TT_STEP_37);
  printf("packets %d\n", TT_PACKETS);
  printf("clip %lu clipdeepest %d\n", (unsigned long)clipCount, clipDeepest);
  uint8_t b[TT_BYTES];
  for (int p = 0; p < TT_PACKETS; p++) {
    makePacket(b, p);
    printf("packet %d bytes", p);
    for (int i = 0; i < TT_BYTES; i++) printf(" %02X", b[i]);
    printf("\n");
    printf("packet %d luma", p);
    for (int i = 0; i < TT_SAMPLES; i++) printf(" %d", wave[p][i]);
    printf("\n");
  }
  return 0;
}
#endif

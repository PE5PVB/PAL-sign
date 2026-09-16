#include "settings.h"

#include <string.h>

#include <Arduino.h>
#include <hardware/flash.h>
#include <hardware/sync.h>

#include "config.h"
#include "flashspeed.h"
#include "photoflash.h"
#include "portrait.h"
#include "adv7391.h"
#include "aspect.h"
#include "display.h"
#include "inserts.h"
#include "language.h"
#include "movingline.h"
#include "teletext.h"
#include "textoverlay.h"
#include "video.h"

// Generous: textPrepare truncates at TEXT_MAX_CHARS itself, so typing
// something too long is allowed and simply comes out cut off.
static const int TEXT_MAX = CFG_TEXT_MAX;

// Cannot be initialised directly: the values in config.h are arrays and
// not string literals. Hence cfgInit().
static char textId[TEXT_MAX];
static char textSub[TEXT_MAX];
static bool g924Balken = CFG_G924_CHROMA_BARS;
static bool tcgAp2 = CFG_TESTCARDG_AP2;
static bool slideFadeOn = CFG_SLIDE_FADE_ON;
static bool dstAutoOn = CFG_DST_AUTO_ON;
static char contestText1[TEXT_MAX];
static char contestText2[TEXT_MAX];
static int contestNumber = CFG_CONTEST_NUMBER;
static int contestScaleText = CFG_CONTEST_SCALE_TEXT;
static int contestScaleNumber = CFG_CONTEST_SCALE_NUMBER;
static bool reload = false;
static bool reloadText = false;  // the light variant: only the text layout
static bool dirty = false;       // changed since the last save
static bool cardDirty = false;   // a flag of its own; see cfgSetStartPattern()


// --- storage in flash -------------------------------------------------
//
// Settings sit in the first sector of the space left over after the
// photograph slots and the two custom card photos (journalOffset(),
// below); see photoFreeOffset() in photoflash.h and portrait.h. Saving is on
// command (key * on the board, Save in the PC tool) and not per change,
// since an erase blacks the picture; the active card is the exception,
// written at once via cfgSaveCard().
//
// Each block carries its own magic, version and length; blockValid()
// checks all three plus a CRC. Growing a block invalidates every record
// already in flash, so new fields go in a new page or a spare byte
// where possible; otherwise a version bump ships a migration path.
static const uint32_t INST_MAGIC = 0x54534E49;  // "INST", little-endian
static const uint16_t INST_VERSIE = 2;

struct SettingsBlock {
  uint32_t magic;
  uint16_t version;
  uint16_t length;  // sizeof(SettingsBlock), so growth can be spotted
  uint32_t crc;     // CRC32 over everything AFTER this field
  char textId[TEXT_MAX];
  char textSub[TEXT_MAX];
  uint8_t g924Balken;
  uint8_t tcgAp2;
  uint8_t insertMode;
  uint8_t contestScaleNumber;
  char contestText1[TEXT_MAX];
  char contestText2[TEXT_MAX];
  uint16_t contestNumber;
  uint8_t contestScaleText;
  // The card to start with. Took the place of a reserve byte, so no
  // version bump was needed; cfgSave() memsets the block to zero first,
  // so an older block reads 0 here, the first card.
  uint8_t startPattern;
};

// A page is the smallest unit flash_range_program can handle and the
// block fits in one easily. Erasing does go per sector.
static const uint32_t PAGINA = 256u;
static_assert(sizeof(SettingsBlock) <= PAGINA, "the block does not fit in one flash page");

// --- the slideshow sequence ------------------------------------------
//
// A list of "how long, which card", written as 2s:1/3s:12/1.5s:13. Any
// card may be in it, not just the uploadable ones, and every step has
// its own time.
//
// Its own page, right behind the settings, with its own magic and CRC:
// growing SettingsBlock would otherwise invalidate every block already
// in flash. Both live in the same sector, so one erase covers both.
static const uint32_t SEQ_MAGIC = 0x51455301;  // "\1SEQ"
static const uint16_t SEQ_VERSION = 1;

struct SeqBlock {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc;   // over everything after this field
  uint16_t count;
  // The save counter of the whole record, not a sequence field: this
  // was padding, sits under the crc already, and reads 0 on a record
  // from before it existed. writePages() stamps it; recScan() orders
  // the ring by it.
  uint16_t save;
  uint16_t ms[SEQ_MAX_STEPS];    // how long the step stands
  uint8_t card[SEQ_MAX_STEPS];   // 0 based index into the card list
  uint8_t pad2[SEQ_MAX_STEPS % 2 ? 1 : 0];
};
static_assert(sizeof(SeqBlock) <= PAGINA, "the sequence does not fit in one flash page");

static int seqCount = 0;
static uint16_t seqMs[SEQ_MAX_STEPS];
static uint8_t seqCard[SEQ_MAX_STEPS];

int cfgSeqCount() { return seqCount; }
uint16_t cfgSeqMs(int i) { return (i >= 0 && i < seqCount) ? seqMs[i] : 0; }
int cfgSeqCard(int i) { return (i >= 0 && i < seqCount) ? seqCard[i] : 0; }

void cfgSeqClear() {
  if (seqCount == 0) return;
  seqCount = 0;
  dirty = true;
}

static uint32_t seqCrc(const SeqBlock *b) {
  const uint8_t *p = (const uint8_t *)b + offsetof(SeqBlock, count);
  uint32_t n = (uint32_t)(sizeof(SeqBlock) - offsetof(SeqBlock, count));
  return photoCrc32(p, n, 0xFFFFFFFFu);
}

// --- the settings journal ---------------------------------------------
//
// One record per sector, in a ring of sectors behind the photo slots
// AND behind the two custom card-photo replacements portrait.h keeps
// there (journalOffset()/journalFreeBytes() start past those). A save
// normally only programs (erase is much slower and blacks the
// picture); only once the ring runs out of blank sectors does a save
// erase its target first. The newest VALID record wins at boot, judged
// by a circular save counter rather than sector number, since the ring
// wraps. Page 0 is programmed last, so a power cut halfway leaves the
// previous record as the newest.
static int recNewest = -1;    // the record the readers point at; -1 = none
static uint16_t recSaveSeq = 0;   // the counter of that record

static uint32_t journalOffset() { return photoFreeOffset() + portraitReservedBytes(); }
static uint32_t journalFreeBytes() { return photoFreeBytes() - portraitReservedBytes(); }

static int recCount() {
  uint32_t n = journalFreeBytes() / FLASH_SECTOR_SIZE;
  if (n > 32) n = 32;   // 32 saves a session before an erase shows
  return (int)n;
}

static const uint8_t *recBase(int k) {
  return (const uint8_t *)(XIP_BASE + journalOffset() + (uint32_t)k * FLASH_SECTOR_SIZE);
}

// What the readers below point at: the newest record, or the first
// sector before anything was ever saved.
static const uint8_t *recActive() { return recBase(recNewest < 0 ? 0 : recNewest); }

static bool flashIsBlank(const uint8_t *p, uint32_t bytes) {
  const uint32_t *w = (const uint32_t *)p;
  for (uint32_t i = 0; i < bytes / 4; i++)
    if (w[i] != 0xFFFFFFFFu) return false;
  return true;
}

static const SeqBlock *seqInFlash() {
  return (const SeqBlock *)(recActive() + PAGINA);
}

static bool seqValid(const SeqBlock *b) {
  if (b->magic != SEQ_MAGIC) return false;
  if (b->version != SEQ_VERSION) return false;
  if (b->length != sizeof(SeqBlock)) return false;
  if (b->count > SEQ_MAX_STEPS) return false;
  return b->crc == seqCrc(b);
}

// Parses "2s:1/3s:12/1.5s:13".
//
// Liberal in what it accepts, since this is typed by hand: spaces
// anywhere, the s may be left off, a comma passes for a decimal point.
// Strict about what matters: the time has to be more than nothing and
// the card has to exist.
//
// Returns the number of steps, or -1 with the reason in `err` and
// nothing changed, so a typing mistake never leaves half a sequence
// behind.
int cfgSeqParse(const char *text, int cardCount, char *err, int errLen) {
  uint16_t ms[SEQ_MAX_STEPS];
  uint8_t card[SEQ_MAX_STEPS];
  int n = 0;

  const char *p = text;
  while (*p == ' ') p++;
  if (!*p) {
    seqCount = 0;
    dirty = true;
    return 0;
  }

  for (;;) {
    while (*p == ' ') p++;

    // The time, in whole thousandths and not floating point: this chip
    // has no hardware double, so a double here would pull in the
    // software routines for one line of parsing.
    long whole = 0, frac = 0;
    bool any = false;
    while (*p >= '0' && *p <= '9') {
      if (whole < 100000) whole = whole * 10 + (*p - '0');
      p++;
      any = true;
    }
    if (*p == '.' || *p == ',') {
      p++;
      int digits = 0;
      while (*p >= '0' && *p <= '9') {
        if (digits < 3) {
          frac = frac * 10 + (*p - '0');
          digits++;
        }
        p++;
        any = true;
      }
      while (digits < 3) {
        frac *= 10;
        digits++;
      }
    }
    if (!any) {
      snprintf(err, (size_t)errLen, LANG_STEP_D_HAS_NO, n + 1);
      return -1;
    }
    while (*p == ' ') p++;
    if (*p == 's' || *p == 'S') p++;
    while (*p == ' ') p++;
    if (*p != ':') {
      snprintf(err, (size_t)errLen, LANG_STEP_D_IS_MISSING, n + 1);
      return -1;
    }
    p++;
    while (*p == ' ') p++;

    int num = 0;
    bool anyNum = false;
    while (*p >= '0' && *p <= '9') {
      num = num * 10 + (*p++ - '0');
      anyNum = true;
      if (num > 9999) break;
    }
    if (!anyNum) {
      snprintf(err, (size_t)errLen, LANG_STEP_D_HAS_NO_2, n + 1);
      return -1;
    }
    if (num < 1 || num > cardCount) {
      snprintf(err, (size_t)errLen, LANG_STEP_D_NAMES_CARD, n + 1, num, cardCount);
      return -1;
    }

    const long msl = whole * 1000 + frac;
    if (msl < 1) {
      snprintf(err, (size_t)errLen, LANG_STEP_D_LASTS_NO, n + 1);
      return -1;
    }
    if (msl > 65535) {
      snprintf(err, (size_t)errLen, LANG_STEP_D_IS_LONGER, n + 1);
      return -1;
    }
    if (n >= SEQ_MAX_STEPS) {
      snprintf(err, (size_t)errLen, LANG_MORE_THAN_D_STEPS, SEQ_MAX_STEPS);
      return -1;
    }
    ms[n] = (uint16_t)msl;
    card[n] = (uint8_t)(num - 1);
    n++;

    while (*p == ' ') p++;
    if (!*p) break;
    if (*p != '/') {
      snprintf(err, (size_t)errLen, LANG_STEP_D_IS_FOLLOWED, n, *p);
      return -1;
    }
    p++;
    // A slash at the very end is forgiven; it is what you get when you
    // delete the last step by hand.
    while (*p == ' ') p++;
    if (!*p) break;
  }

  for (int i = 0; i < n; i++) {
    seqMs[i] = ms[i];
    seqCard[i] = card[i];
  }
  seqCount = n;
  dirty = true;
  return n;
}

// The sequence back as text, in the same form it is read in. Whole
// seconds come out without a decimal point, so 2s and not 2.0s.
void cfgSeqText(char *buf, int len) {
  if (len <= 0) return;
  buf[0] = 0;
  int at = 0;
  for (int i = 0; i < seqCount; i++) {
    const unsigned whole = seqMs[i] / 1000u;
    const unsigned frac = seqMs[i] % 1000u;
    int wrote;
    if (frac == 0) {
      wrote = snprintf(buf + at, (size_t)(len - at), LANG_S_US_D,
                       i ? "/" : "", whole, seqCard[i] + 1);
    } else if (frac % 100 == 0) {
      wrote = snprintf(buf + at, (size_t)(len - at), LANG_S_U_US_D,
                       i ? "/" : "", whole, frac / 100, seqCard[i] + 1);
    } else if (frac % 10 == 0) {
      wrote = snprintf(buf + at, (size_t)(len - at), LANG_S_U_02US_D,
                       i ? "/" : "", whole, frac / 10, seqCard[i] + 1);
    } else {
      wrote = snprintf(buf + at, (size_t)(len - at), LANG_S_U_03US_D,
                       i ? "/" : "", whole, frac, seqCard[i] + 1);
    }
    // snprintf gives what it would have written, so this catches a
    // buffer that has run out; the partial step is cut away. With
    // SEQ_TEXT_MAX sized buffers this cannot trigger.
    if (wrote < 0 || wrote >= len - at) {
      buf[at] = 0;
      return;
    }
    at += wrote;
  }
}

// --- what is set per card --------------------------------------------
//
// Version 1 is not read, every card falls back on config.h. Version 3:
// current card order (CARD_ORDER_V2 maps the old one). Version 4: six
// to eight bytes, band speed/direction. Version 5: Mixed bars enters
// the list (cardShiftV4). Version 6: Mixed bars/Picdream move after TVE
// (CARD_ORDER_V5). Version 7: eight to ten bytes, tickerSize, no
// reorder. Version 8: Crosshatch enters after Multiburst (cardShiftV7).
static const uint32_t CARD_MAGIC = 0x44524143;  // "CARD"
static const uint16_t CARD_VERSION = 8;

// More than there are cards, so adding one does not need a new version.
static const int CARD_SLOTS = 48;

static const uint8_t CARD_SHOW_ID = 0x01;
static const uint8_t CARD_TICKER_MASK = 0x06;   // 0 off, 1 solid, 2 transparent
static const int CARD_TICKER_SHIFT = 1;
// Two bits for the face, 0x08 low and 0x80 high, so a third face fits.
// The high bit was the last spare one; an older block reads 0 there,
// keeping faces 0 and 1 as they were.
static const uint8_t CARD_TICKER_FONT = 0x08;
static const uint8_t CARD_TICKER_FONT2 = 0x80;
static const uint8_t CARD_MOVING_LINE = 0x10;   // the sweeping stripe, see movingline.h
// The second line, inverted: the bit HIDES it, so an older block, which
// reads 0 here, keeps showing both lines as the one switch used to.
static const uint8_t CARD_HIDE_SUB = 0x20;
// Inverted for the same reason: an older block reads 0 here, and that
// has to keep meaning "this card takes part". See cfgCardEnabled().
static const uint8_t CARD_DISABLED = 0x40;

// In the reserved byte below, not flags: flags is full. Only BBC Test
// Card F and W read this; an older block reads 0 here, which keeps
// showing the built-in photo as before. See portrait.h.
static const uint8_t CARD_CUSTOM_PHOTO = 0x01;

// Ten bytes a card, so the table is 480 bytes; still well inside the
// two flash pages the block is checked against below. Nine fields of
// data plus one explicit spare: the uint16_t below forces 2-byte
// alignment, so nine bytes would silently gain the same tenth byte as
// invisible compiler padding. Named and reserved instead, the same
// reasoning as ExtraBlock::spare[] elsewhere in this file.
struct CardSetting {
  uint8_t flags;      // see the masks above
  uint8_t aspect;     // index into ASPECTS[], 0 = no WSS
  uint8_t insert;     // the insert boxes, 0..INSERT_COUNT-1
  // What this card has of its own, read nowhere else: the chroma bars
  // of the PM5644 16:9, the AP2 variant of Test Card G.
  uint8_t option;
  uint16_t tickerY;   // top row of the band
  // The band's speed and direction: negative left, positive right, the
  // size of the number is the rung on the ladder in tickerFrame(),
  // every rung an even sample count. Zero is snapped away by the
  // setter.
  int8_t tickerSpeed;
  // The card's own lettering face, an index into FONTS[]; the ticker's
  // face is separate, in the flags above. Reads 0 (PM5544, the factory
  // face) on a record from before this byte.
  uint8_t textFont;
  // 0 extra small .. 4 extra large, big ticker faces only (Inter,
  // Doto); see ticker.cpp's TICKER_SIZE_COUNT. 0 is a real value, not
  // an unwritten-record marker: every path that fills a CardSetting
  // writes an explicit value.
  uint8_t tickerSize;
  // Was spare, alignment already padded the struct to this width; now
  // CARD_CUSTOM_PHOTO, above. An older block reads 0 here.
  uint8_t reserved;
};
static_assert(sizeof(CardSetting) == 10, "the card record has grown a hole");

// The six byte record of versions 2 and 3, read field for field into
// the grown one; a mapped-across card starts its band walking left at
// the old fixed pace.
struct CardSettingV3 {
  uint8_t flags, aspect, insert, option;
  uint16_t tickerY;
};

// The eight byte record of versions 4, 5 and 6, read field for field
// into the grown one on a card block from before tickerSize existed.
struct CardSettingV6 {
  uint8_t flags, aspect, insert, option;
  uint16_t tickerY;
  int8_t tickerSpeed;
  uint8_t textFont;
};
static_assert(sizeof(CardSettingV6) == 8, "the old card record has grown a hole");

struct CardBlock {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc;   // over everything after this field
  CardSetting card[CARD_SLOTS];
};
static_assert(sizeof(CardBlock) <= 2 * PAGINA, "the card records do not fit in two pages");

// The ticker, in a fourth page. Absent or invalid means off. Version 2
// added outline thickness; 3 moved the mode into the per-card flags; 5
// added per-card colour; 6 added speed. Versions 4/5 are still read for
// their text (tickValidV4/tickValidV5).
static const uint32_t TICK_MAGIC = 0x4B434954;  // "TICK"
static const uint16_t TICK_VERSION = 6;

struct TickerBlock {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc;  // over everything after this field
  // The text and its speed; the face, height and whether it shows at
  // all are kept per card. One message serves them all, at one speed.
  char text[CFG_TICKER_MAX + 1];
  // Dormant: for one build this was the ticker's one global colour,
  // before colours moved per card into the block on pages six to
  // eight. Written white, read only to seed a record with no colour
  // block yet.
  uint8_t colR, colG, colB;
  // Dormant too: the one global speed, before it moved per card.
  // Written 4, never read.
  uint8_t speed;
};
static_assert(sizeof(TickerBlock) <= PAGINA, "the ticker does not fit in one flash page");

// The version 5 and 4 layouts, kept so their text carries into a
// version 6 record on the first save. The colour bytes of a version 5
// block sit at the same offset as in version 6, which cfgInit()'s
// seeding leans on.
struct TickerBlockV5 {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc;
  char text[CFG_TICKER_MAX + 1];
  uint8_t colR, colG, colB;
};

struct TickerBlockV4 {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc;
  char text[CFG_TICKER_MAX + 1];
};

static char tickerTextBuf[CFG_TICKER_MAX + 1];

static uint32_t tickCrc(const TickerBlock *b) {
  const uint8_t *p = (const uint8_t *)b + offsetof(TickerBlock, text);
  uint32_t n = (uint32_t)(sizeof(TickerBlock) - offsetof(TickerBlock, text));
  return photoCrc32(p, n, 0xFFFFFFFFu);
}

static const TickerBlock *tickInFlash() {
  return (const TickerBlock *)(recActive() + 4 * PAGINA);
}

static bool tickValid(const TickerBlock *b) {
  if (b->magic != TICK_MAGIC) return false;
  if (b->version != TICK_VERSION) return false;
  if (b->length != sizeof(TickerBlock)) return false;
  return b->crc == tickCrc(b);
}

static bool tickValidV4(const TickerBlockV4 *b) {
  if (b->magic != TICK_MAGIC) return false;
  if (b->version != 4) return false;
  if (b->length != sizeof(TickerBlockV4)) return false;
  const uint8_t *p = (const uint8_t *)b + offsetof(TickerBlockV4, text);
  uint32_t n = (uint32_t)(sizeof(TickerBlockV4) - offsetof(TickerBlockV4, text));
  return b->crc == photoCrc32(p, n, 0xFFFFFFFFu);
}

static bool tickValidV5(const TickerBlockV5 *b) {
  if (b->magic != TICK_MAGIC) return false;
  if (b->version != 5) return false;
  if (b->length != sizeof(TickerBlockV5)) return false;
  const uint8_t *p = (const uint8_t *)b + offsetof(TickerBlockV5, text);
  uint32_t n = (uint32_t)(sizeof(TickerBlockV5) - offsetof(TickerBlockV5, text));
  return b->crc == photoCrc32(p, n, 0xFFFFFFFFu);
}

// The height and the face are per card, and these four read the card on
// screen, so ticker.cpp needs no card numbers; the PC tool sets a card
// it is not looking at through the cfgCard... pair instead.
int cfgTickerY() { return cfgCardTickerY(-1); }
void cfgSetTickerY(int y) { cfgSetCardTickerY(-1, y); }
int cfgTickerFont() { return cfgCardTickerFont(-1); }
void cfgSetTickerFont(int i) { cfgSetCardTickerFont(-1, i); }
int cfgTickerSpeed() { return cfgCardTickerSpeed(-1); }
int cfgTickerSize() { return cfgCardTickerSize(-1); }

// The fifth page: the two fixed insert-box lines and the seven analogue
// controls of the encoder. Room for sixteen controls while there are
// seven; the count written is stored, so an older page reads back only
// as far as it goes.
static const uint32_t EXTRA_MAGIC = 0x41525458;  // "XTRA"
static const uint16_t EXTRA_VERSION = 4;
static const int EXTRA_CONTROLS = 16;

struct ExtraBlock {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc;   // over everything after this field
  char left[TEXT_MAX];
  char right[TEXT_MAX];
  uint16_t controlCount;
  int16_t control[EXTRA_CONTROLS];
  uint8_t brightness;   // the little panel, 0 to 100
  uint8_t lumaFilter;   // index in the encoder's luma filter list
  uint8_t chromaFilter; // idem for chroma
  uint8_t spare[5];     // room to grow without a new version
  // Version 4: the Ham PCM text channel, appended; a version 3 page
  // (without it) is still read, see extraValidV3().
  char pcmText[PCM_TEXT_MAX];
};
static_assert(sizeof(ExtraBlock) <= PAGINA, "the fifth page does not fit in one flash page");
static const uint32_t EXTRA_V3_LENGTH = offsetof(ExtraBlock, pcmText);

// Defined further down, where the other texts are.
static void setText(char *target, const char *s);

static char insertLeft[TEXT_MAX];
static char insertRight[TEXT_MAX];
static char pcmText[PCM_TEXT_MAX];

static uint32_t extraCrc(const ExtraBlock *b) {
  const uint8_t *p = (const uint8_t *)b + offsetof(ExtraBlock, left);
  uint32_t n = (uint32_t)(sizeof(ExtraBlock) - offsetof(ExtraBlock, left));
  return photoCrc32(p, n, 0xFFFFFFFFu);
}

static const ExtraBlock *extraInFlash() {
  return (const ExtraBlock *)(recActive() + 5 * PAGINA);
}

static bool extraValid(const ExtraBlock *b) {
  if (b->magic != EXTRA_MAGIC) return false;
  if (b->version != EXTRA_VERSION) return false;
  if (b->length != sizeof(ExtraBlock)) return false;
  return b->crc == extraCrc(b);
}

// The page as version 3 wrote it: everything up to pcmText.
static bool extraValidV3(const ExtraBlock *b) {
  if (b->magic != EXTRA_MAGIC) return false;
  if (b->version != 3) return false;
  if (b->length != EXTRA_V3_LENGTH) return false;
  const uint8_t *p = (const uint8_t *)b + offsetof(ExtraBlock, left);
  return b->crc == photoCrc32(p, EXTRA_V3_LENGTH - offsetof(ExtraBlock, left), 0xFFFFFFFFu);
}

// Pages six to eight: the four lettering colours per card, three pages
// of their own (does not fit in the card records). A record from an
// older firmware lacks them and every card starts white. sRGB triples;
// conversion to YCbCr happens where the lettering is laid out (see
// yccFromRgb in gfx.h). Order matches the CFG_COLOR_ indices in
// settings.h. Versions 2-4 track the same card reorders CARD_VERSION
// did (cardShiftV4, cardOrderV5, cardShiftV7).
static const uint32_t COLOR_MAGIC = 0x524C4F43;  // "COLR"
static const uint16_t COLOR_VERSION = 4;

struct CardColors {
  uint8_t rgb[CFG_COLOR_COUNT][3];
};

struct ColorBlock {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc;   // over everything after this field
  CardColors card[CARD_SLOTS];
};
static_assert(sizeof(ColorBlock) <= 3 * PAGINA, "the colours do not fit in three flash pages");

static CardColors colorSet[CARD_SLOTS];

static uint32_t colorCrc(const ColorBlock *b) {
  const uint8_t *p = (const uint8_t *)b + offsetof(ColorBlock, card);
  uint32_t n = (uint32_t)(sizeof(ColorBlock) - offsetof(ColorBlock, card));
  return photoCrc32(p, n, 0xFFFFFFFFu);
}

static const ColorBlock *colorInFlash() {
  return (const ColorBlock *)(recActive() + 6 * PAGINA);
}

static bool colorValid(const ColorBlock *b) {
  if (b->magic != COLOR_MAGIC) return false;
  if (b->version != COLOR_VERSION) return false;
  if (b->length != sizeof(ColorBlock)) return false;
  return b->crc == colorCrc(b);
}

static bool colorValidV3(const ColorBlock *b) {
  if (b->magic != COLOR_MAGIC) return false;
  if (b->version != 3) return false;
  if (b->length != sizeof(ColorBlock)) return false;
  return b->crc == colorCrc(b);
}

static bool colorValidV2(const ColorBlock *b) {
  if (b->magic != COLOR_MAGIC) return false;
  if (b->version != 2) return false;
  if (b->length != sizeof(ColorBlock)) return false;
  return b->crc == colorCrc(b);
}

static bool colorValidV1(const ColorBlock *b) {
  if (b->magic != COLOR_MAGIC) return false;
  if (b->version != 1) return false;
  if (b->length != sizeof(ColorBlock)) return false;
  return b->crc == colorCrc(b);
}

// Pages nine to eleven: the teletext page, three pages of their own
// (TT_ROWS * (TT_COLS + 1) is 615 bytes, past what one page holds). A
// row left empty (a record from before this feature, or never touched)
// falls back to teletext.cpp's own built-in page; see
// teletextApplyRow().
static const uint32_t TELETEXT_MAGIC = 0x54584554;  // "TETX"
static const uint16_t TELETEXT_VERSION = 1;

struct TeletextBlock {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc;   // over everything after this field
  char row[TT_ROWS][TT_COLS + 1];
};
static_assert(sizeof(TeletextBlock) <= 3 * PAGINA, "the teletext page does not fit in three flash pages");

static char teletextRows[TT_ROWS][TT_COLS + 1];

static uint32_t teletextCrc(const TeletextBlock *b) {
  const uint8_t *p = (const uint8_t *)b + offsetof(TeletextBlock, row);
  uint32_t n = (uint32_t)(sizeof(TeletextBlock) - offsetof(TeletextBlock, row));
  return photoCrc32(p, n, 0xFFFFFFFFu);
}

static const TeletextBlock *teletextInFlash() {
  return (const TeletextBlock *)(recActive() + 9 * PAGINA);
}

static bool teletextValid(const TeletextBlock *b) {
  if (b->magic != TELETEXT_MAGIC) return false;
  if (b->version != TELETEXT_VERSION) return false;
  if (b->length != sizeof(TeletextBlock)) return false;
  return b->crc == teletextCrc(b);
}

// How many pages a record is. Everything that fills, blanks, programs
// or carries a record counts with this; the sector has sixteen.
static const int REC_PAGES = 12;

// The little panel; applied by display.cpp, which knows how the pin is
// driven.
int cfgDisplayBrightness() { return displayBrightness(); }

void cfgSetDisplayBrightness(int percent) {
  if (percent == displayBrightness()) return;
  displaySetBrightness(percent);
  dirty = true;
}

const char *cfgPcmText() { return pcmText; }
void cfgSetPcmText(const char *s) {
  if (!strncmp(pcmText, s, PCM_TEXT_MAX - 1)) return;
  strncpy(pcmText, s, PCM_TEXT_MAX - 1);
  pcmText[PCM_TEXT_MAX - 1] = 0;
  reloadText = true;
  dirty = true;
}

const char *cfgInsertLeft() { return insertLeft; }
const char *cfgInsertRight() { return insertRight; }
// The layout is kept until something says otherwise, so a new caption
// only appears once the overlay is told to build itself again.
void cfgSetInsertLeft(const char *s) { setText(insertLeft, s); insertForceRefresh(); }
void cfgSetInsertRight(const char *s) { setText(insertRight, s); insertForceRefresh(); }

const char *cfgTickerText() { return tickerTextBuf; }

void cfgSetTickerText(const char *s) {
  // Unchanged text does not raise the unsaved flag: the PC tool sends
  // the box on every focus leave, the same reason setText() compares.
  char t[CFG_TICKER_MAX + 1];
  strncpy(t, s, CFG_TICKER_MAX);
  t[CFG_TICKER_MAX] = 0;
  if (strcmp(t, tickerTextBuf) == 0) return;
  memcpy(tickerTextBuf, t, sizeof(t));
  dirty = true;
}

const char *cfgTeletextRow(int row) {
  if (row < 1 || row > TT_ROWS) return "";
  return teletextRows[row - 1];
}

void cfgSetTeletextRow(int row, const char *s) {
  if (row < 1 || row > TT_ROWS) return;
  strncpy(teletextRows[row - 1], s, TT_COLS);
  teletextRows[row - 1][TT_COLS] = 0;
  dirty = true;
}


// Which card the per-card settings apply to. Firmware.ino says so at
// every change; this file already knows about card numbers because it
// keeps the one to start on.
static int activeCard = 0;
static CardSetting cardSet[CARD_SLOTS];

// What a card carries, handed over once at startup by the one place
// that holds the card list. Not stored in flash: it follows from which
// card it is, and working it out here would mean a second copy of that
// list.
static uint8_t cardTrait[CARD_SLOTS];

void cfgSetCardTraits(int card, uint8_t t) {
  if (card >= 0 && card < CARD_SLOTS) cardTrait[card] = t;
}

uint8_t cfgCardTraits(int card) {
  return (card >= 0 && card < CARD_SLOTS) ? cardTrait[card] : 0;
}

// Whether the active card has a place for the identification at all:
// seven of the seventeen have not, and switching it there means
// nothing. Nothing declares it; a card asks for the text through
// cfgTextIdShown() while laying itself out, and that is the only
// evidence there is. Cleared at every card change, before anyone can
// press a key.
static bool idAsked = false;
static bool subAsked = false;

// What was seen of each card, so the PC tool can leave a switch out on
// a card with no place for it. 0 means that card has not laid itself
// out here yet; fills in as the tool visits each card to fetch its
// picture, and until then the answer is "not known", which the tool
// treats as "offer it".
static const uint8_t ID_UNKNOWN = 0;
static const uint8_t ID_NO_ROOM = 1;
static const uint8_t ID_HAS_ROOM = 2;
static uint8_t cardIdSeen[CARD_SLOTS];
static uint8_t cardBoxSeen[CARD_SLOTS];
static uint8_t cardLineSeen[CARD_SLOTS];
static uint8_t cardSubSeen[CARD_SLOTS];

bool cfgCardHasId() { return idAsked; }
bool cfgCardHasSub() { return subAsked; }

uint8_t cfgCardIdSeen(int card) {
  if (card < 0 || card >= CARD_SLOTS) return ID_UNKNOWN;
  // The card on screen has laid itself out, so the flag is the
  // freshest answer there is.
  if (card == activeCard) return idAsked ? ID_HAS_ROOM : ID_NO_ROOM;
  if (cardIdSeen[card] != ID_UNKNOWN) return cardIdSeen[card];
  // Not shown yet: fall back on the declared trait rather than wait,
  // since the PC only fetches pictures it lacks a copy of and a card
  // whose copy is still good may never load at all.
  return (cardTrait[card] & CARD_TRAIT_ID) ? ID_HAS_ROOM : ID_NO_ROOM;
}

// The insert boxes: observed through insertPrepare() from the tick of a
// card, written out in setup() for cards that have not had their turn.
uint8_t cfgCardBoxSeen(int card) {
  if (card < 0 || card >= CARD_SLOTS) return ID_UNKNOWN;
  if (card == activeCard && insertInUse()) return ID_HAS_ROOM;
  if (cardBoxSeen[card] != ID_UNKNOWN) return cardBoxSeen[card];
  return (cardTrait[card] & CARD_TRAIT_BOX) ? ID_HAS_ROOM : ID_NO_ROOM;
}

// The moving line, the same story: observed through movingLineBox().
uint8_t cfgCardLineSeen(int card) {
  if (card < 0 || card >= CARD_SLOTS) return ID_UNKNOWN;
  if (card == activeCard && movingLineInUse()) return ID_HAS_ROOM;
  if (cardLineSeen[card] != ID_UNKNOWN) return cardLineSeen[card];
  return (cardTrait[card] & CARD_TRAIT_LINE) ? ID_HAS_ROOM : ID_NO_ROOM;
}

// The second line, the same story: observed through cfgTextSubShown().
uint8_t cfgCardSubSeen(int card) {
  if (card < 0 || card >= CARD_SLOTS) return ID_UNKNOWN;
  if (card == activeCard) return subAsked ? ID_HAS_ROOM : ID_NO_ROOM;
  if (cardSubSeen[card] != ID_UNKNOWN) return cardSubSeen[card];
  return (cardTrait[card] & CARD_TRAIT_SUB) ? ID_HAS_ROOM : ID_NO_ROOM;
}

void cfgSetActiveCard(int i) {
  // Not on the first call: this runs just before the new card's init,
  // so the flag belongs to the card being left, and on the very first
  // call there is none.
  static bool leaving = false;

  // Written down before it is wiped, and checked against the declared
  // trait, so a trait that has rotted out of step says so out loud.
  if (leaving && activeCard >= 0 && activeCard < CARD_SLOTS) {
    const uint8_t seen = idAsked ? ID_HAS_ROOM : ID_NO_ROOM;
    if (cardIdSeen[activeCard] == ID_UNKNOWN &&
        seen != ((cardTrait[activeCard] & CARD_TRAIT_ID) ? ID_HAS_ROOM : ID_NO_ROOM)) {
      Serial.print(LANG_CFG_CARD);
      Serial.print(activeCard + 1);
      Serial.print(seen == ID_HAS_ROOM ? LANG_TRAIT_ID_SEEN : LANG_TRAIT_ID_NOT_SEEN);
      Serial.println(LANG_SEE_CARD_TRAIT_ID);
    }
    cardIdSeen[activeCard] = seen;

    // Read here and not after insertClearInUse(); the order matters,
    // which is why selectPattern() calls this first.
    const uint8_t box = insertInUse() ? ID_HAS_ROOM : ID_NO_ROOM;
    if (cardBoxSeen[activeCard] == ID_UNKNOWN &&
        box != ((cardTrait[activeCard] & CARD_TRAIT_BOX) ? ID_HAS_ROOM : ID_NO_ROOM)) {
      Serial.print(LANG_CFG_CARD);
      Serial.print(activeCard + 1);
      Serial.print(box == ID_HAS_ROOM ? LANG_TRAIT_BOX_SEEN : LANG_TRAIT_BOX_NOT_SEEN);
      Serial.println(LANG_SEE_CARD_TRAIT_BOX);
    }
    cardBoxSeen[activeCard] = box;

    // Read before movingLineClearInUse(), same reason as the boxes above.
    const uint8_t ln = movingLineInUse() ? ID_HAS_ROOM : ID_NO_ROOM;
    if (cardLineSeen[activeCard] == ID_UNKNOWN &&
        ln != ((cardTrait[activeCard] & CARD_TRAIT_LINE) ? ID_HAS_ROOM : ID_NO_ROOM)) {
      Serial.print(LANG_CFG_CARD);
      Serial.print(activeCard + 1);
      Serial.print(ln == ID_HAS_ROOM ? LANG_TRAIT_LINE_SEEN : LANG_TRAIT_LINE_NOT_SEEN);
      Serial.println(LANG_SEE_CARD_TRAIT_LINE);
    }
    cardLineSeen[activeCard] = ln;

    const uint8_t sub = subAsked ? ID_HAS_ROOM : ID_NO_ROOM;
    if (cardSubSeen[activeCard] == ID_UNKNOWN &&
        sub != ((cardTrait[activeCard] & CARD_TRAIT_SUB) ? ID_HAS_ROOM : ID_NO_ROOM)) {
      Serial.print(LANG_CFG_CARD);
      Serial.print(activeCard + 1);
      Serial.print(sub == ID_HAS_ROOM ? LANG_TRAIT_SUB_SEEN : LANG_TRAIT_SUB_NOT_SEEN);
      Serial.println(LANG_SEE_CARD_TRAIT_SUB);
    }
    cardSubSeen[activeCard] = sub;
  }
  leaving = true;
  activeCard = (i >= 0 && i < CARD_SLOTS) ? i : 0;
  idAsked = false;
  subAsked = false;
}

// Every setter takes a card number, -1 meaning the one on screen: the
// board's own keys work on what is showing, while the PC tool sets a
// card it is not looking at.
static int cardIndex(int card) {
  if (card < 0) return activeCard;
  return (card < CARD_SLOTS) ? card : activeCard;
}

static void setFlag(int card, uint8_t mask, bool on) {
  CardSetting &c = cardSet[cardIndex(card)];
  const uint8_t was = c.flags;
  c.flags = on ? (uint8_t)(was | mask) : (uint8_t)(was & ~mask);
  if (c.flags != was) dirty = true;
}

bool cfgShowId() { return (cardSet[activeCard].flags & CARD_SHOW_ID) != 0; }
bool cfgCardShowId(int card) { return (cardSet[cardIndex(card)].flags & CARD_SHOW_ID) != 0; }
void cfgSetShowId(bool on) { setFlag(-1, CARD_SHOW_ID, on); }
void cfgSetCardShowId(int card, bool on) { setFlag(card, CARD_SHOW_ID, on); }

bool cfgShowSub() { return (cardSet[activeCard].flags & CARD_HIDE_SUB) == 0; }
bool cfgCardShowSub(int card) { return (cardSet[cardIndex(card)].flags & CARD_HIDE_SUB) == 0; }
void cfgSetShowSub(bool on) { setFlag(-1, CARD_HIDE_SUB, !on); }
void cfgSetCardShowSub(int card, bool on) { setFlag(card, CARD_HIDE_SUB, !on); }

// BBC Test Card F and W only; see CARD_CUSTOM_PHOTO above and portrait.h.
bool cfgCardCustomPhoto(int card) {
  return (cardSet[cardIndex(card)].reserved & CARD_CUSTOM_PHOTO) != 0;
}
void cfgSetCardCustomPhoto(int card, bool on) {
  CardSetting &c = cardSet[cardIndex(card)];
  const uint8_t was = c.reserved;
  c.reserved = on ? (uint8_t)(was | CARD_CUSTOM_PHOTO) : (uint8_t)(was & ~CARD_CUSTOM_PHOTO);
  if (c.reserved != was) dirty = true;
}

int cfgCardTicker() { return cfgCardTickerOf(-1); }

int cfgCardTickerOf(int card) {
  return (cardSet[cardIndex(card)].flags & CARD_TICKER_MASK) >> CARD_TICKER_SHIFT;
}

void cfgSetCardTicker(int mode) { cfgSetCardTickerOf(-1, mode); }

void cfgSetCardTickerOf(int card, int mode) {
  // Three modes; the two-bit mask would store a nonexistent mode 3.
  if (mode < 0 || mode > 2) return;
  CardSetting &c = cardSet[cardIndex(card)];
  const uint8_t was = c.flags;
  c.flags = (uint8_t)((was & ~CARD_TICKER_MASK) | ((uint8_t)mode << CARD_TICKER_SHIFT));
  if (c.flags != was) dirty = true;
}

int cfgCardTickerFont(int card) {
  const uint8_t f = cardSet[cardIndex(card)].flags;
  return ((f & CARD_TICKER_FONT) ? 1 : 0) | ((f & CARD_TICKER_FONT2) ? 2 : 0);
}

void cfgSetCardTickerFont(int card, int face) {
  setFlag(card, CARD_TICKER_FONT, (face & 1) != 0);
  setFlag(card, CARD_TICKER_FONT2, (face & 2) != 0);
}

bool cfgCardMovingLine(int card) {
  return (cardSet[cardIndex(card)].flags & CARD_MOVING_LINE) != 0;
}

void cfgSetCardMovingLine(int card, bool on) { setFlag(card, CARD_MOVING_LINE, on); }

bool cfgCardEnabled(int card) { return (cardSet[cardIndex(card)].flags & CARD_DISABLED) == 0; }

void cfgSetCardEnabled(int card, bool on) { setFlag(card, CARD_DISABLED, !on); }

int cfgCardTickerY(int card) { return cardSet[cardIndex(card)].tickerY; }

void cfgSetCardTickerY(int card, int y) {
  if (y < 0) y = 0;
  if (y > SCREEN_H) y = SCREEN_H;
  CardSetting &c = cardSet[cardIndex(card)];
  if (c.tickerY == (uint16_t)y) return;
  c.tickerY = (uint16_t)y;
  dirty = true;
}

// The colours of the lettering, four per card; see the enum in
// settings.h for which is which.
uint32_t cfgCardColor(int card, int kind) {
  if (kind < 0 || kind >= CFG_COLOR_COUNT) return 0xFFFFFFu;
  const uint8_t *c = colorSet[cardIndex(card)].rgb[kind];
  return ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
}

void cfgSetCardColor(int card, int kind, uint32_t rgb) {
  if (kind < 0 || kind >= CFG_COLOR_COUNT) return;
  uint8_t *c = colorSet[cardIndex(card)].rgb[kind];
  const uint8_t r = (uint8_t)(rgb >> 16), g = (uint8_t)(rgb >> 8), b = (uint8_t)rgb;
  if (c[0] == r && c[1] == g && c[2] == b) return;
  c[0] = r;
  c[1] = g;
  c[2] = b;
  dirty = true;
}

int cfgCardTickerSpeed(int card) { return cardSet[cardIndex(card)].tickerSpeed; }

void cfgSetCardTickerSpeed(int card, int v) {
  // A standing band is not on offer here; the ticker mode is what turns
  // it off. Zero over the wire snaps to the slowest leftward rung.
  if (v == 0) v = -1;
  if (v < -8) v = -8;
  if (v > 8) v = 8;
  CardSetting &c = cardSet[cardIndex(card)];
  if (c.tickerSpeed == (int8_t)v) return;
  c.tickerSpeed = (int8_t)v;
  dirty = true;
}

int cfgCardTickerSize(int card) { return cardSet[cardIndex(card)].tickerSize; }

void cfgSetCardTickerSize(int card, int v) {
  if (v < TICKER_SIZE_MIN) v = TICKER_SIZE_MIN;
  if (v > TICKER_SIZE_MAX) v = TICKER_SIZE_MAX;
  CardSetting &c = cardSet[cardIndex(card)];
  if (c.tickerSize == (uint8_t)v) return;
  c.tickerSize = (uint8_t)v;
  dirty = true;
}

// The face of the card's own lettering; Firmware.ino checks the index
// against its font count, this only keeps what fits the byte.
int cfgCardTextFont(int card) { return cardSet[cardIndex(card)].textFont; }

void cfgSetCardTextFont(int card, int v) {
  if (v < 0) v = 0;
  if (v > 7) v = 7;
  CardSetting &c = cardSet[cardIndex(card)];
  if (c.textFont == (uint8_t)v) return;
  c.textFont = (uint8_t)v;
  dirty = true;
}

int cfgCardAspect(int card) { return cardSet[cardIndex(card)].aspect; }

void cfgSetCardAspect(int card, int v) {
  if (v < 0 || v >= ASPECT_COUNT) return;
  CardSetting &c = cardSet[cardIndex(card)];
  if (c.aspect == (uint8_t)v) return;
  c.aspect = (uint8_t)v;
  dirty = true;
}

int cfgCardInsert(int card) { return cardSet[cardIndex(card)].insert; }

void cfgSetCardInsert(int card, int v) {
  if (v < 0 || v >= INSERT_COUNT) return;
  CardSetting &c = cardSet[cardIndex(card)];
  if (c.insert == (uint8_t)v) return;
  c.insert = (uint8_t)v;
  dirty = true;
}

int cfgCardOption(int card) { return cardSet[cardIndex(card)].option; }

void cfgSetCardOption(int card, int v) {
  CardSetting &c = cardSet[cardIndex(card)];
  if (c.option == (uint8_t)v) return;
  c.option = (uint8_t)v;
  dirty = true;
}

// A key for the picture the PC keeps, over what decides it (card,
// build, settings, shared texts) rather than rendering it to check.
// The clock is left out: cards with insert boxes would otherwise
// change every second and never cache.
static const char BUILD_ID[] = __DATE__ " " __TIME__;

uint32_t cfgCardCrc(int card) {
  const int i = cardIndex(card);
  uint32_t crc = 0xFFFFFFFFu;
  // See CFG_CARD_CRC_BUILD_ID in config.h for why this can be left out.
  if (CFG_CARD_CRC_BUILD_ID)
    crc = photoCrc32((const uint8_t *)BUILD_ID, (uint32_t)sizeof(BUILD_ID), crc);
  const uint8_t idx = (uint8_t)i;
  crc = photoCrc32(&idx, 1, crc);
  // Not the whole record: the ticker (the PC draws it itself) and the
  // aspect ratio (the PC applies its own letterbox/squeeze) are left
  // out. Insert mode stays in, it switches the EPROM variant on the
  // Philips cards.
  const uint8_t bare[2] = {
      (uint8_t)(cardSet[i].flags & (CARD_SHOW_ID | CARD_HIDE_SUB)),
      cardSet[i].insert,
  };
  crc = photoCrc32(bare, sizeof(bare), crc);
  crc = photoCrc32(&cardSet[i].option, 1, crc);

  // Only what can actually appear on this card: a shared text that went
  // into every card's checksum would make all thirty-three copies on
  // the PC out of date at once and force a refetch of every one of
  // them. Every condition below is in the checksum itself too, through
  // `bare` and the traits, so switching one on brings the text back
  // into the sum.
  if (cardSet[i].flags & CARD_SHOW_ID) {
    crc = photoCrc32((const uint8_t *)textId, TEXT_MAX, crc);
  }
  if (!(cardSet[i].flags & CARD_HIDE_SUB) && (cardTrait[i] & CARD_TRAIT_SUB)) {
    crc = photoCrc32((const uint8_t *)textSub, TEXT_MAX, crc);
  }
  if (cardTrait[i] & CARD_TRAIT_CONTEST) {
    crc = photoCrc32((const uint8_t *)contestText1, TEXT_MAX, crc);
    crc = photoCrc32((const uint8_t *)contestText2, TEXT_MAX, crc);
    const uint8_t nr[2] = {(uint8_t)contestNumber, (uint8_t)(contestNumber >> 8)};
    crc = photoCrc32(nr, sizeof(nr), crc);
  }
  if (cardSet[i].insert == INSERT_TEXT) {
    crc = photoCrc32((const uint8_t *)insertLeft, TEXT_MAX, crc);
    crc = photoCrc32((const uint8_t *)insertRight, TEXT_MAX, crc);
  }
  if (cardTrait[i] & CARD_TRAIT_G924) {
    const uint8_t v = g924Balken ? 1 : 0;
    crc = photoCrc32(&v, 1, crc);
  }
  if (cardTrait[i] & CARD_TRAIT_CARDG) {
    const uint8_t v = tcgAp2 ? 1 : 0;
    crc = photoCrc32(&v, 1, crc);
  }

  // The face goes in for all of them: it is the lettering of every text
  // on this card, and whether the card carries any text is not known
  // here.
  crc = photoCrc32(&cardSet[i].textFont, 1, crc);
  return crc ^ 0xFFFFFFFFu;
}

static uint32_t cardCrc(const CardBlock *b) {
  const uint8_t *p = (const uint8_t *)b + offsetof(CardBlock, card);
  uint32_t n = (uint32_t)(sizeof(CardBlock) - offsetof(CardBlock, card));
  return photoCrc32(p, n, 0xFFFFFFFFu);
}

static const CardBlock *cardInFlash() {
  return (const CardBlock *)(recActive() + 2 * PAGINA);
}

static bool cardValid(const CardBlock *b) {
  if (b->magic != CARD_MAGIC) return false;
  if (b->version != CARD_VERSION) return false;
  if (b->length != sizeof(CardBlock)) return false;
  return b->crc == cardCrc(b);
}

// The two page block as versions 2 and 3 wrote it, with the six byte
// records; version 2 also counted the cards in the old order. cfgInit()
// reads either field for field, mapping version 2 across with
// CARD_ORDER_V2.
struct CardBlockV3 {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc;
  CardSettingV3 card[CARD_SLOTS];
};

static bool cardValidOld(const CardBlockV3 *b, uint16_t ver) {
  if (b->magic != CARD_MAGIC) return false;
  if (b->version != ver) return false;
  if (b->length != sizeof(CardBlockV3)) return false;
  const uint8_t *p = (const uint8_t *)b + offsetof(CardBlockV3, card);
  uint32_t n = (uint32_t)(sizeof(CardBlockV3) - offsetof(CardBlockV3, card));
  return b->crc == photoCrc32(p, n, 0xFFFFFFFFu);
}

// The two page block as versions 4, 5 and 6 wrote it, with the eight
// byte records, one byte narrower than the current CardSetting since
// none of the three ever heard of tickerSize.
struct CardBlockV6 {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc;
  CardSettingV6 card[CARD_SLOTS];
};

static bool cardCrcOkV6(const CardBlockV6 *b) {
  const uint8_t *p = (const uint8_t *)b + offsetof(CardBlockV6, card);
  uint32_t n = (uint32_t)(sizeof(CardBlockV6) - offsetof(CardBlockV6, card));
  return b->crc == photoCrc32(p, n, 0xFFFFFFFFu);
}

// A version 4 block: the same eight byte records as version 5, only
// counted before the Mixed bars card entered the list.
static bool cardValidV4(const CardBlockV6 *b) {
  if (b->magic != CARD_MAGIC) return false;
  if (b->version != 4) return false;
  if (b->length != sizeof(CardBlockV6)) return false;
  return cardCrcOkV6(b);
}

// A version 5 block: the same eight byte records again, only counted
// before Mixed bars and Picdream moved to right after the TVE card.
static bool cardValidV5(const CardBlockV6 *b) {
  if (b->magic != CARD_MAGIC) return false;
  if (b->version != 5) return false;
  if (b->length != sizeof(CardBlockV6)) return false;
  return cardCrcOkV6(b);
}

// A version 6 block: the same eight byte records, the current order,
// only one byte narrower than today's CardSetting: see tickerSize's
// own note there.
static bool cardValidV6(const CardBlockV6 *b) {
  if (b->magic != CARD_MAGIC) return false;
  if (b->version != 6) return false;
  if (b->length != sizeof(CardBlockV6)) return false;
  return cardCrcOkV6(b);
}

// A version 7 block: today's ten byte records already (tickerSize grew
// the record, no reorder), only counted before Crosshatch entered the
// list; see cardShiftV7 below. Uses the current CardBlock/cardCrc(),
// not the eight byte CardBlockV6 shape the three checks above share.
static bool cardValidV7(const CardBlock *b) {
  if (b->magic != CARD_MAGIC) return false;
  if (b->version != 7) return false;
  if (b->length != sizeof(CardBlock)) return false;
  return b->crc == cardCrc(b);
}

// Old (version 5) position to new (version 6) position, for Mixed bars
// and Picdream moving to right after the TVE card.
// Index is the old position, value is the new one; positions beyond 34
// do not exist yet and read as identity.
static const uint8_t CARD_ORDER_V5[35] = {
    0,  1,  2,  3,  4,  5,  6,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 7,
    19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 8};

static uint8_t cardOrderV5(uint8_t i) { return i < 35 ? CARD_ORDER_V5[i] : i; }

// Old position to new position for the Mixed bars card entering at
// position 17 (0 based), between the contest page and the photo slots:
// the seventeen drawn cards stay put, everything behind moves one up.
static uint8_t cardShiftV4(uint8_t i) { return i < 17 ? i : (uint8_t)(i + 1); }

// Old (version 7) position to new (version 8) position, for Crosshatch
// entering at position 18 (0 based), right after Multiburst: the
// eighteen cards up to and including Multiburst stay put, Contest and
// every photo slot behind it move one up.
static uint8_t cardShiftV7(uint8_t i) { return i < 18 ? i : (uint8_t)(i + 1); }

// The speed a record from before the speed byte gets: walking left at
// the pace the band always had, rung 4 of the ladder.
static const int8_t TICKER_SPEED_OLD = -4;

// The size a record from before tickerSize existed gets: medium, the
// one size every big face drew at before this feature existed.
static const uint8_t TICKER_SIZE_OLD = 2;

static void cardFromV3(CardSetting *dst, const CardSettingV3 *src) {
  dst->flags = src->flags;
  dst->aspect = src->aspect;
  dst->insert = src->insert;
  dst->option = src->option;
  dst->tickerY = src->tickerY;
  dst->tickerSpeed = TICKER_SPEED_OLD;
  dst->textFont = 0;
  dst->tickerSize = TICKER_SIZE_OLD;
  dst->reserved = 0;
}

static void cardFromV6(CardSetting *dst, const CardSettingV6 *src) {
  dst->flags = src->flags;
  dst->aspect = src->aspect;
  dst->insert = src->insert;
  dst->option = src->option;
  dst->tickerY = src->tickerY;
  dst->tickerSpeed = src->tickerSpeed;
  dst->textFont = src->textFont;
  dst->tickerSize = TICKER_SIZE_OLD;
  dst->reserved = 0;
}


// Old position to new position for the seventeen drawn cards, from
// before Firmware.ino's current order; the sixteen photo slots behind
// them kept their places.
static const uint8_t CARD_ORDER_V2[17] = {0, 7, 11, 2, 1, 8, 4, 9,
                                          6, 5, 10, 13, 14, 15, 12, 3, 16};

static uint8_t cardOrderV2(uint8_t i) { return i < 17 ? CARD_ORDER_V2[i] : i; }

static uint32_t blockCrc(const SettingsBlock *b) {
  const uint8_t *p = (const uint8_t *)b + offsetof(SettingsBlock, textId);
  uint32_t n = (uint32_t)(sizeof(SettingsBlock) - offsetof(SettingsBlock, textId));
  return photoCrc32(p, n, 0xFFFFFFFFu);
}

static const SettingsBlock *blockInFlash() {
  return (const SettingsBlock *)recActive();
}

static bool blockValid(const SettingsBlock *b) {
  if (b->magic != INST_MAGIC) return false;
  if (b->version != INST_VERSIE) return false;
  if (b->length != sizeof(SettingsBlock)) return false;
  return b->crc == blockCrc(b);
}

// The boot scan: the highest sector whose main block validates is the
// newest record, since that page is programmed last; the other blocks
// keep their own checks at load time.
static void recScan() {
  recNewest = -1;
  recSaveSeq = 0;
  for (int k = 0; k < recCount(); k++) {
    if (!blockValid((const SettingsBlock *)recBase(k))) continue;
    const SeqBlock *q = (const SeqBlock *)(recBase(k) + PAGINA);
    const uint16_t seq = seqValid(q) ? q->save : 0;
    // Newer or equal wins; on equal counters the later sector wins,
    // which orders records from a firmware without the counter.
    // Circular, so the counter's own wrap cannot misorder the handful
    // of records ever live at once.
    if (recNewest >= 0 && (uint16_t)(seq - recSaveSeq) >= 0x8000u) continue;
    recNewest = k;
    recSaveSeq = seq;
  }
}

bool cfgStorageValid() { return blockValid(blockInFlash()); }
bool cfgDirty() { return dirty; }
bool cfgCardDirty() { return cardDirty; }

// For settings that live somewhere else: the encoder's analogue
// controls are kept and ranged in adv7391.cpp, but a copy goes into
// flash here, so turning one has to make the Save button light up like
// anything else.
void cfgTouch() { dirty = true; }

// The insert boxes. Shared, since both the G00 and the G924 have them
// (G00 as a black box, G924 as a thin two-row line) and both use them
// to pick their EPROM variant.
static int insertMode = CFG_INSERT_MODE;

// The card the board comes up on. See settings.h.
static int startPattern = CFG_PATTERN;

int cfgStartPattern() { return startPattern; }

// In the top bit of the startPattern byte rather than a field of its
// own, which would move `length` and invalidate every block already in
// flash. The card index fits easily (the sequence stores it in a
// uint8 too), so bit 7 can never be part of a card number, and an
// older block holds 0 there, "not running".
static bool slideshowSaved = false;

bool cfgSlideshowOn() { return slideshowSaved; }

// Rides the same at-once write as the card itself (cardDirty), since it
// answers the same question: what should the board do when the power
// comes back?
void cfgSetSlideshowOn(bool on) {
  if (on == slideshowSaved) return;
  slideshowSaved = on;
  cardDirty = true;
}

// A flag of its own, kept apart from `dirty`: which card is showing
// goes into flash at once, so a board switched off right after a
// change comes back up on it, while everything else waits for the save
// command. Were this to set `dirty` too, a card change would carry
// every other unsaved setting into flash with it, since one record
// carries all the pages together.
void cfgSetStartPattern(int v) {
  // Unchanged is not dirty: otherwise going back and forth between two
  // cards would keep asking for a write that changes nothing.
  if (v == startPattern) return;
  startPattern = v;
  cardDirty = true;
}

static void zetFabriek() {
  strncpy(textId, CFG_TEXT_ID, TEXT_MAX - 1);
  strncpy(textSub, CFG_TEXT_SUB, TEXT_MAX - 1);
  textId[TEXT_MAX - 1] = 0;
  textSub[TEXT_MAX - 1] = 0;
  g924Balken = CFG_G924_CHROMA_BARS;
  tcgAp2 = CFG_TESTCARDG_AP2;
  slideFadeOn = CFG_SLIDE_FADE_ON;
  dstAutoOn = CFG_DST_AUTO_ON;
  insertMode = CFG_INSERT_MODE;
  strncpy(pcmText, CFG_PCM_TEXT, PCM_TEXT_MAX - 1);
  pcmText[PCM_TEXT_MAX - 1] = 0;
  strncpy(contestText1, CFG_CONTEST_TEXT1, TEXT_MAX - 1);
  strncpy(contestText2, CFG_CONTEST_TEXT2, TEXT_MAX - 1);
  contestText1[TEXT_MAX - 1] = 0;
  contestText2[TEXT_MAX - 1] = 0;
  contestNumber = CFG_CONTEST_NUMBER;
  contestScaleText = CFG_CONTEST_SCALE_TEXT;
  contestScaleNumber = CFG_CONTEST_SCALE_NUMBER;
  startPattern = CFG_PATTERN;
  slideshowSaved = false;
  for (int i = 0; i < CARD_SLOTS; i++) {
    cardSet[i].flags = CARD_SHOW_ID;   // every card shows it until told otherwise
    cardSet[i].aspect = CFG_ASPECT;
    cardSet[i].insert = CFG_INSERT_MODE;
    cardSet[i].option = 0;
    cardSet[i].tickerY = CFG_TICKER_Y;
    cardSet[i].tickerSpeed = TICKER_SPEED_OLD;
    cardSet[i].textFont = 0;
    cardSet[i].tickerSize = CFG_TICKER_SIZE;
    cardSet[i].reserved = 0;
  }
  strncpy(tickerTextBuf, CFG_TICKER_TEXT, CFG_TICKER_MAX);
  tickerTextBuf[CFG_TICKER_MAX] = 0;
  memset(teletextRows, 0, sizeof(teletextRows));   // empty: teletext.cpp's own default page stands
  memset(colorSet, 0xFF, sizeof(colorSet));   // all white: 255,255,255 everywhere
  strncpy(insertLeft, CFG_TEXT_INSERT_LEFT, TEXT_MAX - 1);
  insertLeft[TEXT_MAX - 1] = 0;
  strncpy(insertRight, CFG_TEXT_INSERT_RIGHT, TEXT_MAX - 1);
  insertRight[TEXT_MAX - 1] = 0;
}

void cfgInit() {
  zetFabriek();
  // Which record of the journal is the newest; the readers below all
  // point at it through recActive().
  recScan();
  const SettingsBlock *b = blockInFlash();
  if (!blockValid(b)) return;  // blank or foreign flash: the factory setting stays

  memcpy(textId, b->textId, TEXT_MAX);
  memcpy(textSub, b->textSub, TEXT_MAX);
  textId[TEXT_MAX - 1] = 0;
  textSub[TEXT_MAX - 1] = 0;
  g924Balken = b->g924Balken != 0;
  tcgAp2 = b->tcgAp2 != 0;
  if (b->insertMode < INSERT_COUNT) insertMode = b->insertMode;
  memcpy(contestText1, b->contestText1, TEXT_MAX);
  memcpy(contestText2, b->contestText2, TEXT_MAX);
  contestText1[TEXT_MAX - 1] = 0;
  contestText2[TEXT_MAX - 1] = 0;
  if (b->contestNumber <= CONTEST_NUMBER_MAX) contestNumber = b->contestNumber;
  if (b->contestScaleText >= 1) contestScaleText = b->contestScaleText;
  if (b->contestScaleNumber >= 1) contestScaleNumber = b->contestScaleNumber;
  // No upper bound here: this file does not know how many cards there
  // are. Firmware.ino checks it against its own list and falls back on
  // the first one, the same as an empty photo slot does.
  startPattern = b->startPattern & 0x7F;
  slideshowSaved = (b->startPattern & 0x80) != 0;

  const CardBlock *c = cardInFlash();
  const CardBlockV3 *c3 = (const CardBlockV3 *)c;
  const CardBlockV6 *c6 = (const CardBlockV6 *)c;
  if (cardValid(c)) {
    memcpy(cardSet, c->card, sizeof(cardSet));
  } else if (cardValidV7(c)) {
    // Already today's ten byte records, only the order changes: a
    // plain copy per slot, no field conversion, unlike cardFromV6
    // below.
    for (int i = 0; i < CARD_SLOTS - 1; i++) {
      cardSet[cardShiftV7((uint8_t)i)] = c->card[i];
    }
  } else if (cardValidV6(c6)) {
    for (int i = 0; i < CARD_SLOTS - 1; i++) {
      cardFromV6(&cardSet[cardShiftV7((uint8_t)i)], &c6->card[i]);
    }
  } else if (cardValidV5(c6)) {
    for (int i = 0; i < CARD_SLOTS - 1; i++) {
      cardFromV6(&cardSet[cardShiftV7(cardOrderV5((uint8_t)i))], &c6->card[i]);
    }
  } else if (cardValidV4(c6)) {
    // Two shifts chain here (cardShiftV4 then cardShiftV7), each able to
    // push an index one past the last real slot, so this one loop needs
    // to stop a slot earlier than the others do; see CARD_SLOTS's own
    // note on why there is room to spare.
    for (int i = 0; i < CARD_SLOTS - 2; i++) {
      cardFromV6(&cardSet[cardShiftV7(cardOrderV5(cardShiftV4((uint8_t)i)))], &c6->card[i]);
    }
  } else if (cardValidOld(c3, 3)) {
    for (int i = 0; i < CARD_SLOTS - 2; i++) {
      cardFromV3(&cardSet[cardShiftV7(cardOrderV5(cardShiftV4((uint8_t)i)))], &c3->card[i]);
    }
  } else if (cardValidOld(c3, 2)) {
    for (int i = 0; i < CARD_SLOTS - 2; i++) {
      cardFromV3(&cardSet[cardShiftV7(cardOrderV5(cardShiftV4(cardOrderV2((uint8_t)i))))],
                &c3->card[i]);
    }
  }

  const SeqBlock *q = seqInFlash();
  if (seqValid(q)) {
    seqCount = (int)q->count;
    for (int i = 0; i < seqCount; i++) {
      seqMs[i] = q->ms[i];
      seqCard[i] = q->card[i];
    }
  }

  // startPattern and seqCard also count cards by position, so they need
  // the same maps the card block above took: version 2 or nothing takes
  // all four; 3 and 4 take the shift plus the two newest; 5 takes the
  // two newest; 6 and 7 take only the newest, cardShiftV7. Zero maps to
  // zero, so a factory fresh flash is unaffected.
  if (!cardValid(c)) {
    if (!cardValidV7(c) && !cardValidV6(c6)) {
      if (!cardValidV4(c6) && !cardValidV5(c6) && !cardValidOld(c3, 3)) {
        startPattern = cardOrderV2((uint8_t)startPattern);
        for (int i = 0; i < seqCount; i++) seqCard[i] = cardOrderV2(seqCard[i]);
      }
      if (!cardValidV5(c6)) {
        startPattern = cardShiftV4((uint8_t)startPattern);
        for (int i = 0; i < seqCount; i++) seqCard[i] = cardShiftV4(seqCard[i]);
      }
      startPattern = cardOrderV5((uint8_t)startPattern);
      for (int i = 0; i < seqCount; i++) seqCard[i] = cardOrderV5(seqCard[i]);
    }
    startPattern = cardShiftV7((uint8_t)startPattern);
    for (int i = 0; i < seqCount; i++) seqCard[i] = cardShiftV7(seqCard[i]);
  }

  const TickerBlock *tk = tickInFlash();
  if (tickValid(tk)) {
    memcpy(tickerTextBuf, tk->text, sizeof(tickerTextBuf));
    tickerTextBuf[CFG_TICKER_MAX] = 0;
  } else if (tickValidV5((const TickerBlockV5 *)tk)) {
    // A record from before the speed: the text carries across, and the
    // block goes out as version 6 on the first save.
    memcpy(tickerTextBuf, ((const TickerBlockV5 *)tk)->text, sizeof(tickerTextBuf));
    tickerTextBuf[CFG_TICKER_MAX] = 0;
  } else if (tickValidV4((const TickerBlockV4 *)tk)) {
    // The same, one step older: from before the colour bytes.
    memcpy(tickerTextBuf, ((const TickerBlockV4 *)tk)->text, sizeof(tickerTextBuf));
    tickerTextBuf[CFG_TICKER_MAX] = 0;
  }

  const ColorBlock *cb = colorInFlash();
  if (colorValid(cb)) {
    memcpy(colorSet, cb->card, sizeof(colorSet));
  } else if (colorValidV3(cb)) {
    // From before Crosshatch entered right after Multiburst: the
    // colours belong to card positions, so they take the card records'
    // newest reorder.
    for (int i = 0; i < CARD_SLOTS - 1; i++) {
      colorSet[cardShiftV7((uint8_t)i)] = cb->card[i];
    }
  } else if (colorValidV2(cb)) {
    // From before Mixed bars and Picdream moved to right after TVE: the
    // colours belong to card positions, so they take the card records'
    // reorder.
    for (int i = 0; i < CARD_SLOTS - 1; i++) {
      colorSet[cardShiftV7(cardOrderV5((uint8_t)i))] = cb->card[i];
    }
  } else if (colorValidV1(cb)) {
    // From before the Mixed bars card: the same shift as the card
    // records, then the same reorders as the branches above. Two
    // shifts chain here, so this loop stops a slot earlier; see the
    // same note at the CARD_VERSION 4 branch above.
    for (int i = 0; i < CARD_SLOTS - 2; i++) {
      colorSet[cardShiftV7(cardOrderV5(cardShiftV4((uint8_t)i)))] = cb->card[i];
    }
  } else if (tickValid(tk) || tickValidV5((const TickerBlockV5 *)tk)) {
    // A record from the one build with a single global ticker colour:
    // that becomes the ticker colour of every card, so it is not lost
    // in the move to per card. The colour bytes sit at the same offset
    // in version 5 and 6.
    for (int i = 0; i < CARD_SLOTS; i++) {
      colorSet[i].rgb[CFG_COLOR_TICKER][0] = tk->colR;
      colorSet[i].rgb[CFG_COLOR_TICKER][1] = tk->colG;
      colorSet[i].rgb[CFG_COLOR_TICKER][2] = tk->colB;
    }
  }

  const ExtraBlock *xb = extraInFlash();
  if (extraValid(xb)) {
    memcpy(pcmText, xb->pcmText, PCM_TEXT_MAX);
    pcmText[PCM_TEXT_MAX - 1] = 0;
  }
  if (extraValid(xb) || extraValidV3(xb)) {
    memcpy(insertLeft, xb->left, TEXT_MAX);
    memcpy(insertRight, xb->right, TEXT_MAX);
    insertLeft[TEXT_MAX - 1] = 0;
    insertRight[TEXT_MAX - 1] = 0;
    // Set and not written: the encoder is still in reset here, and
    // advBringUp() puts all seven out at the end anyway.
    int n = xb->controlCount;
    if (n > EXTRA_CONTROLS) n = EXTRA_CONTROLS;
    if (n > advParamCount()) n = advParamCount();
    for (int i = 0; i < n; i++) advParamLoad(i, xb->control[i]);
    // Safe this early: displayBegin() has not run yet, and it asks for
    // the level itself when it puts the backlight on.
    displaySetBrightness(xb->brightness);
    advLoadFilters(xb->lumaFilter, xb->chromaFilter);
    // spare[0], inverted like CARD_DISABLED: fillPages() zeroes the
    // block first, so a block from before this feature reads 0 here,
    // and 0 has to keep meaning the fade stays on.
    slideFadeOn = xb->spare[0] == 0;
    // spare[1], not inverted this time: the old behaviour (no summer
    // time awareness) already reads as 0. See clock.cpp.
    dstAutoOn = xb->spare[1] != 0;
  }

  const TeletextBlock *tt = teletextInFlash();
  if (teletextValid(tt)) {
    memcpy(teletextRows, tt->row, sizeof(teletextRows));
    for (int i = 0; i < TT_ROWS; i++) teletextRows[i][TT_COLS] = 0;
  }
  dirty = false;
}

void cfgFactoryReset() {
  zetFabriek();
  seqCount = 0;
  reload = true;
  dirty = true;
}

// Erasing and programming.
//
// Same precaution as in photoflash.cpp: videoSuspend() has to succeed
// first, so core1 runs out of RAM with interrupts off and touches
// nothing in the XIP space, while core0's interrupts are off around
// this.
//
// Only one sector is erased and one page programmed, short compared
// with a photograph's 204 sectors. How long exactly is not looked up:
// VideoStats::flashLines counts the black rows that go out meanwhile
// (each 64 us), so the interruption is measured, not assumed.
// The whole sector as it should become, out of what is in RAM.
static void fillPages(uint8_t *page) {
  SettingsBlock b;
  memset(&b, 0, sizeof(b));
  b.magic = INST_MAGIC;
  b.version = INST_VERSIE;
  b.length = sizeof(b);
  memcpy(b.textId, textId, TEXT_MAX);
  memcpy(b.textSub, textSub, TEXT_MAX);
  b.g924Balken = g924Balken ? 1 : 0;
  b.tcgAp2 = tcgAp2 ? 1 : 0;
  b.insertMode = (uint8_t)insertMode;
  memcpy(b.contestText1, contestText1, TEXT_MAX);
  memcpy(b.contestText2, contestText2, TEXT_MAX);
  b.contestNumber = (uint16_t)contestNumber;
  b.contestScaleText = (uint8_t)contestScaleText;
  b.contestScaleNumber = (uint8_t)contestScaleNumber;
  b.startPattern = (uint8_t)(startPattern | (slideshowSaved ? 0x80 : 0));
  b.crc = blockCrc(&b);

  // Two pages: the settings in the first, the sequence in the second.
  // flash_range_program wants a page multiple anyway, and the erase
  // below wipes the whole sector, so the sequence has to be rewritten
  // with it or it would be lost after every save.
  SeqBlock q;
  memset(&q, 0, sizeof(q));
  q.magic = SEQ_MAGIC;
  q.version = SEQ_VERSION;
  q.length = sizeof(q);
  q.count = (uint16_t)seqCount;
  for (int i = 0; i < seqCount; i++) {
    q.ms[i] = seqMs[i];
    q.card[i] = seqCard[i];
  }
  q.crc = seqCrc(&q);

  CardBlock c;
  memset(&c, 0, sizeof(c));
  c.magic = CARD_MAGIC;
  c.version = CARD_VERSION;
  c.length = sizeof(c);
  memcpy(c.card, cardSet, sizeof(c.card));
  c.crc = cardCrc(&c);

  TickerBlock tk;
  memset(&tk, 0, sizeof(tk));
  tk.magic = TICK_MAGIC;
  tk.version = TICK_VERSION;
  tk.length = sizeof(tk);
  memcpy(tk.text, tickerTextBuf, sizeof(tk.text));
  tk.colR = tk.colG = tk.colB = 255;   // dormant, see the struct
  tk.speed = 4;                        // dormant as well, see the struct
  tk.crc = tickCrc(&tk);

  ExtraBlock xb;
  memset(&xb, 0, sizeof(xb));
  xb.magic = EXTRA_MAGIC;
  xb.version = EXTRA_VERSION;
  xb.length = sizeof(xb);
  memcpy(xb.left, insertLeft, TEXT_MAX);
  memcpy(xb.right, insertRight, TEXT_MAX);
  // The values themselves live in adv7391.cpp, which knows the range of
  // each; this only keeps a copy.
  int nc = advParamCount();
  if (nc > EXTRA_CONTROLS) nc = EXTRA_CONTROLS;
  xb.controlCount = (uint16_t)nc;
  for (int i = 0; i < nc; i++) xb.control[i] = (int16_t)advParamValue(i);
  xb.brightness = (uint8_t)displayBrightness();
  xb.lumaFilter = (uint8_t)advLumaFilterIndex();
  xb.chromaFilter = (uint8_t)advChromaFilterIndex();
  xb.spare[0] = slideFadeOn ? 0 : 1;   // see the note at cfgInit()'s own read of it
  memcpy(xb.pcmText, pcmText, PCM_TEXT_MAX);
  xb.spare[1] = dstAutoOn ? 1 : 0;     // see the note at cfgInit()'s own read of it
  xb.crc = extraCrc(&xb);

  ColorBlock col;
  memset(&col, 0, sizeof(col));
  col.magic = COLOR_MAGIC;
  col.version = COLOR_VERSION;
  col.length = sizeof(col);
  memcpy(col.card, colorSet, sizeof(col.card));
  col.crc = colorCrc(&col);

  TeletextBlock tt;
  memset(&tt, 0, sizeof(tt));
  tt.magic = TELETEXT_MAGIC;
  tt.version = TELETEXT_VERSION;
  tt.length = sizeof(tt);
  memcpy(tt.row, teletextRows, sizeof(tt.row));
  tt.crc = teletextCrc(&tt);

  memset(page, 0xFF, REC_PAGES * PAGINA);
  memcpy(page, &b, sizeof(b));
  memcpy(page + PAGINA, &q, sizeof(q));
  memcpy(page + 2 * PAGINA, &c, sizeof(c));
  memcpy(page + 4 * PAGINA, &tk, sizeof(tk));
  memcpy(page + 5 * PAGINA, &xb, sizeof(xb));
  memcpy(page + 6 * PAGINA, &col, sizeof(col));
  memcpy(page + 9 * PAGINA, &tt, sizeof(tt));
}

// Did the last save have to erase after all? For the report: a save
// that only programs shows a thin flicker at most, one that erases
// blacks the picture visibly.
static bool lastSaveErased = false;
bool cfgLastSaveErased() { return lastSaveErased; }

// Writes the six pages into the next sector of the journal ring. The
// caller decides what is in them; erases only when that sector is not
// blank, which the boot maintenance normally prevents. Page 0 goes
// last; see the journal note at the top.
static bool writePages(uint8_t *page) {
  // flash_range_erase erases per sector and demands a sector-boundary
  // address. That should always hold, but erasing at the wrong address
  // would hit a photo slot, so it is checked rather than assumed.
  uint32_t base = journalOffset();
  if (base % FLASH_SECTOR_SIZE) return false;
  const int n = recCount();
  if (n < 1) return false;
  const int target = (recNewest < 0) ? 0 : (recNewest + 1) % n;
  const uint32_t off = base + (uint32_t)target * FLASH_SECTOR_SIZE;
  const bool blank = flashIsBlank(recBase(target), REC_PAGES * PAGINA);
  lastSaveErased = !blank;

  // The save counter, one higher than the record this replaces, so
  // recScan() can order a wrapped ring. Stamped here and not in
  // fillPages(), because cfgSaveCard() carries the sequence page over
  // from flash word for word and would carry the old counter with it;
  // the crc of that page covers the counter, so it is redone.
  {
    SeqBlock *q = (SeqBlock *)(page + PAGINA);
    q->save = (uint16_t)(recSaveSeq + 1);
    q->crc = seqCrc(q);
  }

  if (!videoSuspend()) return false;
  if (!blank) {
    noInterrupts();
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    interrupts();
  }
  noInterrupts();
  flash_range_program(off + PAGINA, page + PAGINA, (REC_PAGES - 1) * PAGINA);
  interrupts();
  noInterrupts();
  flash_range_program(off, page, PAGINA);
  interrupts();

  // The QSPI clock has to be set again after this: flash_range_erase
  // and flash_range_program restore XIP by running boot2 again
  // (flash_enable_xip_via_boot2 in the SDK's flash.c), and boot2
  // programs qmi_hw->m[0].timing with its own default, slower than the
  // divisor set here. Only the custom photographs notice, since they
  // are the only ones reading 1440 bytes per row straight from flash
  // and need 22.5 MB/s for it; without this line they stutter after a
  // save. The SDK preserves m[1].timing, the PSRAM side, but not m[0].
  flashSpeedApply();

  videoResume();

  // Compared against what is really in flash, not against what was
  // meant to be written. The readers move to the new record first and
  // back again if it does not hold up, so a failed save leaves the
  // previous record in charge.
  const int wasNewest = recNewest;
  recNewest = target;
  if (!blockValid(blockInFlash()) || !seqValid(seqInFlash()) || !cardValid(cardInFlash()) ||
      !tickValid(tickInFlash()) || !extraValid(extraInFlash()) ||
      !colorValid(colorInFlash()) || !teletextValid(teletextInFlash())) {
    recNewest = wasNewest;
    // The half record must not win recScan() at the next boot: the
    // boot scan validates only the main block, so a record whose
    // other pages failed verification would still load on its higher
    // counter, with the broken blocks silently falling back to
    // defaults. Blank it, so the previous record stays newest in
    // flash too.
    if (videoSuspend()) {
      noInterrupts();
      flash_range_erase(off, FLASH_SECTOR_SIZE);
      interrupts();
      flashSpeedApply();
      videoResume();
    }
    return false;
  }
  recSaveSeq = (uint16_t)(recSaveSeq + 1);
  return true;
}

// One pass at the end of bring-up, while the screen is still dark:
// every sector of the ring except the newest record's goes back to
// blank, so the session's own saves only have to program and never
// flash the picture. The first boot after an update erases the ring
// once (about 1.5 s); after that only what the previous session used.
void cfgJournalMaintenance() {
  const int n = recCount();
  bool suspended = false;
  int erased = 0;
  for (int k = 0; k < n; k++) {
    if (k == recNewest) continue;
    if (flashIsBlank(recBase(k), FLASH_SECTOR_SIZE)) continue;
    if (!suspended) {
      if (!videoSuspend()) return;   // core1 not ready: next boot tries again
      suspended = true;
    }
    noInterrupts();
    flash_range_erase(journalOffset() + (uint32_t)k * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
    interrupts();
    erased++;
  }
  if (suspended) {
    flashSpeedApply();
    videoResume();
  }
  if (erased) Serial.printf(LANG_STORAGE_JOURNAL_D_OF_D, erased, n);
}

static uint8_t pageBuf[REC_PAGES * PAGINA];

bool cfgSave() {
  if (!dirty && !cardDirty) return true;
  fillPages(pageBuf);
  if (!writePages(pageBuf)) return false;
  dirty = false;
  cardDirty = false;
  return true;
}

// Which card is showing, and nothing else: every other page is taken
// from what is already in flash and written back word for word, so a
// change waiting on the save command stays waiting. Cannot be a write
// of one page, since a single erase covers the whole sector and the
// other five have to go back with it regardless; the only question is
// whether they come from flash or from RAM, and here it is flash.
bool cfgSaveCard() {
  if (!cardDirty) return true;

  const uint8_t *inFlash = recActive();
  if (!blockValid((const SettingsBlock *)inFlash)) {
    // Nothing to preserve: no valid block yet, so what is in RAM is the
    // only answer there is.
    return cfgSave();
  }

  // From RAM first, then whatever is worth keeping over it. A page
  // valid in flash is copied back byte for byte; one that is not (a
  // firmware update moved its version number) has nothing worth
  // keeping, so the RAM version stands. Writing a stale page back
  // verbatim was a trap: writePages() checks all six afterwards, the
  // stale page failed, the save reported failure with the card still
  // dirty, and the frame hook came straight back for another erase.
  fillPages(pageBuf);
  if (blockValid((const SettingsBlock *)inFlash))
    memcpy(pageBuf, inFlash, PAGINA);
  if (seqValid(seqInFlash()))
    memcpy(pageBuf + PAGINA, inFlash + PAGINA, PAGINA);
  if (cardValid(cardInFlash()))
    memcpy(pageBuf + 2 * PAGINA, inFlash + 2 * PAGINA, 2 * PAGINA);
  if (tickValid(tickInFlash()))
    memcpy(pageBuf + 4 * PAGINA, inFlash + 4 * PAGINA, PAGINA);
  if (extraValid(extraInFlash()))
    memcpy(pageBuf + 5 * PAGINA, inFlash + 5 * PAGINA, PAGINA);
  if (colorValid(colorInFlash()))
    memcpy(pageBuf + 6 * PAGINA, inFlash + 6 * PAGINA, 3 * PAGINA);
  if (teletextValid(teletextInFlash()))
    memcpy(pageBuf + 9 * PAGINA, inFlash + 9 * PAGINA, 3 * PAGINA);

  SettingsBlock *b = (SettingsBlock *)pageBuf;
  b->startPattern = (uint8_t)(startPattern | (slideshowSaved ? 0x80 : 0));
  b->crc = blockCrc(b);
  if (!writePages(pageBuf)) return false;
  cardDirty = false;
  return true;
}

const char *cfgTextId() { return textId; }
const char *cfgTextSub() { return textSub; }

// Empty when this card has its identification off. The text itself stays
// stored, so switching it back on brings the same call sign back.
const char *cfgTextIdShown() {
  idAsked = true;
  return cfgShowId() ? textId : "";
}

const char *cfgTextSubShown() {
  subAsked = true;
  return cfgShowSub() ? textSub : "";
}
bool cfgG924ChromaBars() { return g924Balken; }
bool cfgTestcardgAp2() { return tcgAp2; }
bool cfgSlideFadeOn() { return slideFadeOn; }
bool cfgDstAutoOn() { return dstAutoOn; }

// The same text is not a change: setting one asks for the active card
// to be built again, and the PC tool sends a text box every time the
// cursor leaves it, so without this check the picture would flicker
// past a field nobody had typed in.
static void setText(char *target, const char *s) {
  if (!strncmp(target, s, TEXT_MAX - 1)) return;
  strncpy(target, s, TEXT_MAX - 1);
  target[TEXT_MAX - 1] = 0;
  // Text only: the light reload is enough, no card tables move.
  reloadText = true;
  dirty = true;
}

void cfgSetTextId(const char *s) { setText(textId, s); }
void cfgSetTextSub(const char *s) { setText(textSub, s); }

void cfgSetG924ChromaBars(bool v) {
  if (v == g924Balken) return;   // unchanged is not a card change
  g924Balken = v;
  reload = true;
  dirty = true;
}

void cfgSetTestcardgAp2(bool v) {
  if (v == tcgAp2) return;
  tcgAp2 = v;
  reload = true;
  dirty = true;
}

void cfgSetSlideFadeOn(bool v) {
  // No reload: unlike the two switches above, this touches nothing the
  // active card draws, only how slideshowStep() moves between two of
  // them.
  if (v == slideFadeOn) return;
  slideFadeOn = v;
  dirty = true;
}

void cfgSetDstAutoOn(bool v) {
  // No reload either, and nothing to tell clock.cpp: it reads
  // cfgDstAutoOn() itself on every clockGet()/clockSet(), so this takes
  // effect on the next read.
  if (v == dstAutoOn) return;
  dstAutoOn = v;
  dirty = true;
}

int insertGetMode() { return insertMode; }

const char *insertModeName(int m) {
  switch (m) {
    case INSERT_NONE: return LANG_INSERT_NONE;
    case INSERT_TIME: return LANG_INSERT_TIME;
    case INSERT_DATE_TIME: return LANG_INSERT_DATE_TIME;
    case INSERT_TEXT: return LANG_INSERT_TEXT;
    default: return "?";
  }
}

// Applies the mode, does not store it: the per-card value is what is
// stored, via cfgSetCardInsert(). Called from selectPattern() on every
// card change, so setting `dirty` here made the board claim unsaved
// settings after nothing but switching cards, and a reload made the new
// card's init run a second time 400 ms later.
void insertSetMode(int m) {
  if (m < 0 || m >= INSERT_COUNT) return;
  if (m == insertMode) return;
  insertMode = m;
  reload = true;
}

const char *cfgContestText1() { return contestText1; }
const char *cfgContestText2() { return contestText2; }
int cfgContestNumber() { return contestNumber; }
int cfgContestScaleText() { return contestScaleText; }
int cfgContestScaleNumber() { return contestScaleNumber; }

void cfgSetContestText1(const char *s) { setText(contestText1, s); }
void cfgSetContestText2(const char *s) { setText(contestText2, s); }

void cfgSetContestNumber(int v) {
  if (v < 0) v = 0;
  if (v > CONTEST_NUMBER_MAX) v = CONTEST_NUMBER_MAX;
  if (v == contestNumber) return;
  contestNumber = v;
  reload = true;
  dirty = true;
}

void cfgReload() { reload = true; }
bool cfgReloadNeeded() { return reload; }
// The full init lays the text out as well, so a pending light request
// is answered by it too.
void cfgReloadDone() { reload = false; reloadText = false; }

void cfgReloadText() { reloadText = true; }
bool cfgReloadTextNeeded() { return reloadText; }
void cfgReloadTextDone() { reloadText = false; }

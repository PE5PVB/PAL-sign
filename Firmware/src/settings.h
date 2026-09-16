// Settings changeable over the serial port that survive a restart.
// config.h holds the factory default; a valid block in flash overrides
// it. Nothing is written to flash until asked for (key * on the board,
// Save in the PC tool) except the active card, saved at once.
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

void cfgInit();

const char *cfgTextId();
const char *cfgTextSub();

// Empty when the active card has identification switched off.
const char *cfgTextIdShown();
const char *cfgTextSubShown();

// --- what is set per card --------------------------------------------
void cfgSetActiveCard(int i);

bool cfgShowId();
void cfgSetShowId(bool on);
bool cfgShowSub();
void cfgSetShowSub(bool on);

// 0 off, 1 own bar, 2 over the card; see ticker.h.
int cfgCardTicker();
void cfgSetCardTicker(int mode);

// Card variants of the above: pass -1 for the active card.
bool cfgCardShowId(int card);
void cfgSetCardShowId(int card, bool on);
bool cfgCardShowSub(int card);
void cfgSetCardShowSub(int card, bool on);

// BBC Test Card F and W only: show the uploaded replacement photo
// instead of the built-in one. See portrait.h.
bool cfgCardCustomPhoto(int card);
void cfgSetCardCustomPhoto(int card, bool on);
int cfgCardTickerOf(int card);
void cfgSetCardTickerOf(int card, int mode);
int cfgCardTickerY(int card);
void cfgSetCardTickerY(int card, int y);
int cfgCardTickerFont(int card);
void cfgSetCardTickerFont(int card, int face);

// Only meaningful on a card with a box for it; see movingline.h.
bool cfgCardMovingLine(int card);
void cfgSetCardMovingLine(int card, bool on);

// Whether the card takes part at all: skipped by p, P, the knob and the
// slideshow fallback, but still reachable by number.
bool cfgCardEnabled(int card);
void cfgSetCardEnabled(int card, bool on);
int cfgCardAspect(int card);
void cfgSetCardAspect(int card, int v);
int cfgCardInsert(int card);
void cfgSetCardInsert(int card, int v);

// Whatever that single card has of its own: PM5644 16:9 chroma bars,
// Test Card G AP2.
int cfgCardOption(int card);
void cfgSetCardOption(int card, int v);

// So the PC tool knows when to re-fetch a card's picture.
uint32_t cfgCardCrc(int card);

bool cfgCardHasId();
bool cfgCardHasSub();

// For a card not on screen: 0 not known yet, 1 no, 2 yes.
uint8_t cfgCardIdSeen(int card);
uint8_t cfgCardBoxSeen(int card);    // the insert boxes
uint8_t cfgCardLineSeen(int card);   // the moving line
uint8_t cfgCardSubSeen(int card);    // the second text line

bool cfgG924ChromaBars();
bool cfgTestcardgAp2();

bool cfgSlideFadeOn();
bool cfgDstAutoOn();

void cfgSetTextId(const char *s);
void cfgSetTextSub(const char *s);
void cfgSetG924ChromaBars(bool v);
void cfgSetTestcardgAp2(bool v);
void cfgSetSlideFadeOn(bool v);
void cfgSetDstAutoOn(bool v);

// --- contest page ----------------------------------------------------
static const int CONTEST_NUMBER_MAX = 9999;

const char *cfgContestText1();
const char *cfgContestText2();
int cfgContestNumber();
int cfgContestScaleText();
int cfgContestScaleNumber();

void cfgSetContestText1(const char *s);
void cfgSetContestText2(const char *s);
void cfgSetContestNumber(int v);

// --- the card to start with ------------------------------------------
int cfgStartPattern();
void cfgSetStartPattern(int v);

bool cfgSlideshowOn();
void cfgSetSlideshowOn(bool on);

// --- the slideshow sequence ------------------------------------------
// "how long, which card", e.g. 2s:1/3s:12/1.5s:13.
static const int SEQ_MAX_STEPS = 32;
static const int SEQ_TEXT_MAX = SEQ_MAX_STEPS * 11 + 1;

int cfgSeqCount();
uint16_t cfgSeqMs(int i);
int cfgSeqCard(int i);
void cfgSeqClear();

// Returns the step count, or -1 with the reason in err.
int cfgSeqParse(const char *text, int cardCount, char *err, int errLen);
void cfgSeqText(char *buf, int len);

// --- the ticker: one line of text scrolling across the picture -------
static const int CFG_TICKER_MAX = 96;

int cfgTickerY();
void cfgSetTickerY(int y);

// Index into FONTS[]. Out of range reads back as 0.
int cfgTickerFont();
void cfgSetTickerFont(int i);

static const int CFG_TEXT_MAX = 32;

int cfgDisplayBrightness();
void cfgSetDisplayBrightness(int percent);

const char *cfgInsertLeft();
// The Ham PCM card's text channel: up to PCM_TEXT_MAX - 1 characters.
static const int PCM_TEXT_MAX = 11;
const char *cfgPcmText();
void cfgSetPcmText(const char *s);
const char *cfgInsertRight();
void cfgSetInsertLeft(const char *s);
void cfgSetInsertRight(const char *s);

const char *cfgTickerText();
void cfgSetTickerText(const char *s);

// One teletext row (1..TT_ROWS, see teletext.h). An empty string means
// untouched: teletextApplyRow() then falls back to the built-in page.
const char *cfgTeletextRow(int row);
void cfgSetTeletextRow(int row, const char *s);

// -8..-1 left, 1..8 right, zero not offered (snapped to -1).
int cfgCardTickerSpeed(int card);
void cfgSetCardTickerSpeed(int card, int v);
int cfgTickerSpeed();

// Which of the five fixed sizes (Inter/Doto only): 0 extra small ..
// 4 extra large. Meaningless on the raster faces.
static const int TICKER_SIZE_MIN = 0;
static const int TICKER_SIZE_MAX = 4;
int cfgCardTickerSize(int card);
void cfgSetCardTickerSize(int card, int v);
int cfgTickerSize();

// Index into FONTS[] for the card's own lettering.
int cfgCardTextFont(int card);
void cfgSetCardTextFont(int card, int v);

// Four per-card lettering colours, packed 0xRRGGBB.
enum {
  CFG_COLOR_TICKER = 0,
  CFG_COLOR_ID,      // the first line of the identification
  CFG_COLOR_SUB,     // the second line
  CFG_COLOR_INSERT,  // the insert boxes: date and time, or the fixed texts
  CFG_COLOR_COUNT,
};
uint32_t cfgCardColor(int card, int kind);   // card -1 = the one on screen
void cfgSetCardColor(int card, int kind, uint32_t rgb);

// Parks core1, so the picture blacks briefly. Returns false if the
// readback does not match what was written.
bool cfgSave();
void cfgJournalMaintenance();
bool cfgLastSaveErased();
bool cfgSaveCard();
bool cfgStorageValid();

// What a card carries, so cfgCardCrc() can leave out shared text that
// cannot appear on it.
static const uint8_t CARD_TRAIT_CONTEST = 0x01;    // the contest page
static const uint8_t CARD_TRAIT_G924 = 0x02;       // chroma test bars
static const uint8_t CARD_TRAIT_CARDG = 0x04;      // AP1 and AP2
static const uint8_t CARD_TRAIT_ID = 0x08;         // identification box (observed)
static const uint8_t CARD_TRAIT_BOX = 0x10;        // insert boxes (observed)
static const uint8_t CARD_TRAIT_LINE = 0x20;       // moving-line box (observed)
static const uint8_t CARD_TRAIT_SUB = 0x40;        // second text line (observed)
static const uint8_t CARD_TRAIT_FIXEDFONT = 0x80;  // its own lettering is not a font choice
void cfgSetCardTraits(int card, uint8_t traits);
uint8_t cfgCardTraits(int card);

bool cfgDirty();
bool cfgCardDirty();
void cfgTouch();
void cfgFactoryReset();

void cfgReload();

// Only the active card's text layout redoes, not its tables; falls
// back to cfgReload() on a card without a reflow path.
void cfgReloadText();
bool cfgReloadTextNeeded();
void cfgReloadTextDone();

bool cfgReloadNeeded();
void cfgReloadDone();
#endif  // SETTINGS_H

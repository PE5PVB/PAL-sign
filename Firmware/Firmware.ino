// PAL-sign, the main tab: setup(), loop(), setup1(), loop1(), serial
// commands, the card list. Rest of the firmware is under src/.
// sketch.yaml sets the board profile ("palsign"): Olimex Pico2XXL,
// 16MB flash (3MB sketch / 13MB FS, fixed -- photos and settings live
// in the FS part via _FS_start/_FS_end). CPU speed: setup() sets its
// own clock, must be a multiple of 54 MHz; see CFG_SYSCLK_HZ in
// src/config.h.

// Philips test card generator for the RP2350 and an ADV7391, on an
// Olimex RP2350-PICO2-XXL. PAL B/G composite (CVBS) over BT.656 with
// embedded sync, from the original EPROM dumps of a Philips PM5644.
//
// Wiring (connector J1, "DATA"); board.h has the pin map.
//
//   J1     ADV7391      Olimex XXL
//   1..8   P0..P7       GP0..GP7    pixel bus
//   9      CLKIN        GP12        through JP1, DATA side (pins 2-3)
//   10     ~HSYNC       GP13        also PCM1808 SCKI, see below
//   11     ~VSYNC       GP14        held high, see below
//   12     SFL          GP18        held low, see below
//   13     ~RESET       GP15
//   14     SCL          GP17        I2C0
//   15     SDA          GP16        I2C0
//   16     GND          GND
//
// GP8 is off limits (PSRAM chip select); GP9-11 carry the PCM1808
// experiment now (BCK/LRCK/DOUT, see board.h and src/pcm1808.h), so
// there is no free pin left for the microSD that was never built.
//
// HSYNC/VSYNC held high: unused in slave Timing Mode 0 (datasheet
// p.72 requires unused VSYNC/HSYNC HIGH; safe to drive hard only
// because they stay inputs in this mode, p.16). GP13/~HSYNC has been
// handed to pio1 instead, as the PCM1808's system clock: still safe
// for the same reason, and confirmed further by applyOperatingConfig()
// in adv7391.cpp never touching register 0x02 (SD sync output stays at
// its reset default, off), so the encoder never drives this pin either.
//
// SFL held low (timing reset mode, register 0x84 bits[2:1] = 10): a
// pulse resets the encoder's field counter to Field 1, subcarrier
// phase zero (p.46); see videoRequestSflPulse().
#include <Arduino.h>
#include <hardware/watchdog.h>   // watchdog_caused_reboot(), the factory-reset gate
#include <stdarg.h>

#include <hardware/clocks.h>
#include <hardware/regs/addressmap.h>

#include "src/adv7391.h"
#include "src/aspect.h"
#include "src/board.h"
#include "src/clock.h"
#include "src/config.h"
#include "src/display.h"
#include "src/encoder.h"
#include "src/flashspeed.h"
#include "src/gfx.h"
#include "src/photoflash.h"
#include "src/portrait.h"
#include "src/inserts.h"
#include "src/its.h"
#include "src/language.h"
#include "src/movingline.h"
#include "src/pattern.h"
#include "src/pcm1808.h"
#include "src/asrc.h"
#include "src/rompattern.h"
#include "src/rv3028.h"
#include "src/settings.h"
#include "src/teletext.h"
#include "src/textoverlay.h"
#include "src/ticker.h"
#include "src/video.h"
#include "src/videoloop.h"

// Order is stored by position: per-card settings, start card and
// sequence steps all reference a card by its index here. Reordering
// or inserting needs a new CARD_VERSION and a migration in
// settings.cpp (CARD_ORDER_V2, cardShiftV4, CARD_ORDER_V5, cardShiftV7).
static const Pattern *const PATTERNS[] = {
    &PATTERN_PM5644G00,  &PATTERN_FUBKNOCIRCLE, &PATTERN_FUBK4X3,   &PATTERN_BARSRED,
    &PATTERN_TESTCARDF,  &PATTERN_TESTCARDG,    &PATTERN_TVE,
    &PATTERN_MIXEDBAR,   &PATTERN_PICDREAM,
    &PATTERN_PM5644G924, &PATTERN_FUBK16X9,     &PATTERN_TESTCARDW, &PATTERN_INTERNBAR,
    &PATTERN_PM5644G913, &PATTERN_EBUBW,        &PATTERN_PULSEBAR,  &PATTERN_SINXX,
    &PATTERN_MULTIBURST, &PATTERN_CROSSHATCH,   &PATTERN_CONTEST,
    &PATTERN_CUSTOM1,      &PATTERN_CUSTOM2,   &PATTERN_CUSTOM3,
    &PATTERN_CUSTOM4,      &PATTERN_CUSTOM5,    &PATTERN_CUSTOM6,    &PATTERN_CUSTOM7,
    &PATTERN_CUSTOM8,      &PATTERN_CUSTOM9,    &PATTERN_CUSTOM10,   &PATTERN_CUSTOM11,
    &PATTERN_CUSTOM12,     &PATTERN_CUSTOM13,   &PATTERN_CUSTOM14,
    &PATTERN_PCMAUDIO, &PATTERN_SONYPCM};
static const int PATTERN_COUNT = (int)(sizeof(PATTERNS) / sizeof(PATTERNS[0]));

// The worst row build time since the last timing report; see the
// report for why the lifetime figures are not enough.
static volatile uint32_t timingWindowWorstUs = 0;
static_assert(CFG_PATTERN >= 0 && CFG_PATTERN < PATTERN_COUNT, "CFG_PATTERN out of range");

const Pattern *activePattern = PATTERNS[CFG_PATTERN];
static int patternIndex = CFG_PATTERN;
static bool timingOn = CFG_TIMING_REPORT;

// True while the card dump job is streaming a picture to the PC.
static bool dumpActive = false;

// Only writes when the USB buffer can take it without blocking (a
// blocking write here drops the picture), and never during a dump, or
// the report would land inside the picture bytes.
static void report(const char *fmt, ...) {
  if (dumpActive) return;
  char msg[200];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (Serial && n > 0 && n < (int)sizeof(msg) && Serial.availableForWrite() >= n) {
    Serial.write(msg, n);
  }
}

// A card change costs nearly 10 ms with no rows built, against a ~4 ms
// ring buffer, so a key press is only noted and carried out in the
// frame loop, at most once per SWITCH_MIN_MS; extra presses are dropped.
static int requestedPattern = -1;

// What the encoder is pointing at, or -1 for the card already showing.
// The picture changes on press, not on turn.
static int encoderCandidate = -1;

// millis() of the last turn; a candidate untouched for
// CANDIDATE_TIMEOUT_MS is cancelled. Meaningful only while
// encoderCandidate >= 0.
static uint32_t encoderCandidateLastMs = 0;
static const uint32_t CANDIDATE_TIMEOUT_MS = 3000;

// How long a card change may wait for the ring buffer to refill after a
// write (zero: not waiting).
static const uint32_t SWITCH_WAIT_MAX_MS = 500;
static uint32_t switchWaitMs = 0;
static uint32_t lastSwitchMs = 0;
static const uint32_t SWITCH_MIN_MS = 400;

// Whether a card's ROM tables copy in one go or need pieces, once known;
// found out the first time each card is switched to, kept for the rest
// of the run. See romLoad().
static bool cardCopyInOneGo[PATTERN_COUNT];

// State of a switch still running, so the report waits until the copy
// has actually finished.
static VideoStats switchBefore;
static uint32_t switchStartUs = 0;
static bool switchRunning = false;
static uint32_t switchLastStarves = 0;

// Prints how the last switch went.
static void reportSwitch() {
  VideoStats now;
  videoGetStats(&now);
  switchRunning = false;
  // A switch a flash write ran into empties the ring regardless of the
  // card, so that must not count against it.
  if (romCopyPanicked() && now.flashLines == switchBefore.flashLines) {
    cardCopyInOneGo[patternIndex] = true;
  }
  report(LANG_CARD_D_D_S_LU,
         patternIndex + 1, PATTERN_COUNT, activePattern->name,
         (unsigned long)(micros() - switchStartUs),
         (unsigned long)(now.starves - switchBefore.starves),
         (unsigned long)(now.txStalls - switchBefore.txStalls),
         romCopyForced() ? LANG_COPIED_IN_ONE_GO : LANG_COPIED_IN_PIECES);
}

// Enabled, and, for a photograph slot, actually holding a picture: an
// empty slot always reads as off regardless of the stored flag. Shared
// by nextPattern(), the "[card]" dump's enabled= field, and the panel's
// card count.
// The two PCM sound cards carry the PCM1808's audio and are silence
// without it; when the boot probe found no ADC (pcm1808Present()) they
// disappear completely: not selectable, not listed to the PC tool, not
// in the panel menu, not counted. Everything else is always available.
static bool cardIsAvailable(int i) {
  if (PATTERNS[i] != &PATTERN_PCMAUDIO && PATTERNS[i] != &PATTERN_SONYPCM) return true;
  return pcm1808Present();
}

static bool cardIsEnabled(int i) {
  const int slot = customSlotOf(PATTERNS[i]);
  return cardIsAvailable(i) && cfgCardEnabled(i) && (slot < 0 || photoValid(slot));
}

// How many cards cardIsEnabled() counts, and index's rank among them,
// board order, 1 based: the panel's "N/M".
static int enabledPatternCount() {
  int n = 0;
  for (int i = 0; i < PATTERN_COUNT; i++) {
    if (cardIsEnabled(i)) n++;
  }
  return n;
}

static int enabledPatternRank(int index) {
  int n = 0;
  for (int i = 0; i <= index; i++) {
    if (cardIsEnabled(i)) n++;
  }
  return n;
}

// The next card in that direction, skipping empty photograph slots and
// disabled cards; typing the number still reaches either.
static int nextPattern(int from, int step) {
  int i = from;
  for (int n = 0; n < PATTERN_COUNT; n++) {
    i = (i + step + PATTERN_COUNT) % PATTERN_COUNT;
    if (cardIsEnabled(i)) return i;
  }
  return from;
}

// THIS DOES NOT WRITE ANYTHING; remembering the choice is the caller's
// business, a picture earlier (see the card change in videoFrameHook()).
static void selectPattern(int i) {
  if (i < 0 || i >= PATTERN_COUNT) return;

  encoderCandidate = -1;

  // A switch whose copy has not finished yet is overtaken by this one;
  // report it first so its figures are not lost or mixed with the next.
  if (switchRunning) reportSwitch();
  switchLastStarves = 0;

  videoGetStats(&switchBefore);
  switchStartUs = micros();
  switchRunning = true;

  // Let go of the miniature before init(), so a card that draws its own
  // only has to say so, never to undo it; see displayThumbHold().
  displayThumbHold(false);

  // Applied before init(): a card lays its text out around these, and
  // insertClearInUse() below reads what the card being left just drew.
  cfgSetActiveCard(i);

  insertClearInUse();
  movingLineClearInUse();
  aspectSet(cfgCardAspect(-1));
  insertSetMode(cfgCardInsert(-1));
  // An uploaded photograph reads its rows out of flash; a transparent
  // band on top does not fit the row budget.
  tickerAllowTransparent(customSlotOf(PATTERNS[i]) < 0);
  textInkRefresh();
  textSetFont(cfgCardTextFont(-1));
  tickerBegin();

  PATTERNS[i]->init();
  // A reload asked for on the way here is already answered by the
  // init() just done; without this the frame hook would run it again.
  cfgReloadDone();
  activePattern = PATTERNS[i];
  patternIndex = i;
  advSetMonochrome(PATTERNS[i]->mono);
  advSetColourBars(PATTERNS[i]->encoderMakesIt);

  // Known already unable to keep up, so skip spending rows to find out
  // again.
  if (cardCopyInOneGo[i]) romCopyFinish();

  // A card that cost nothing is done here; everything else waits for
  // videoFrameHook(), which reports as soon as rows stop coming late.
  VideoStats now;
  videoGetStats(&now);
  if (!romCopyPending() && now.starves == switchBefore.starves) {
    reportSwitch();
  } else {
    switchLastStarves = now.starves;
  }
}

// --- the slideshow ---------------------------------------------------
//
// Steps through the filled photograph slots, with a short fade to black
// between them. The fade is done by the encoder (advSetFade(), three
// registers in the Y/Cb/Cr scaling after sync), not in the rows, which
// have no time to spare for it. The card is switched at full black, so
// a heavy card's late rows while it copies fall inside the fade.
static const uint32_t SLIDE_HOLD_MS = 2000;

// One step a picture, 25/s: 240 ms down, 400 ms up. Fade in is longer,
// since that is the one you watch.
static const int SLIDE_FADE_OUT_STEPS = 6;
static const int SLIDE_FADE_IN_STEPS = 10;

// Caps the wait at black so a card that never settles cannot leave the
// slideshow stuck in the dark. 25 pictures is a second.
static const int SLIDE_WAIT_MAX = 25;

enum SlidePhase { SLIDE_SHOWING, SLIDE_OUT, SLIDE_BLACK, SLIDE_IN };

static bool slideshowOn = false;
static SlidePhase slidePhase = SLIDE_SHOWING;
static uint32_t slideHoldUntil = 0;
static int slideStep = 0;
static int slideWaited = 0;
static bool slideSwitched = false;
static int slideSeqIndex = -1;     // where in the sequence, -1 = not started
static uint32_t slideCurrentMs = SLIDE_HOLD_MS;

// The next filled, enabled photograph slot after `from`, or -1: the
// fallback when no sequence is given.
static int nextFilledCustom(int from) {
  for (int n = 0; n < PATTERN_COUNT; n++) {
    const int i = (from + 1 + n) % PATTERN_COUNT;
    if (!cfgCardEnabled(i)) continue;
    const int slot = customSlotOf(PATTERNS[i]);
    if (slot >= 0 && photoValid(slot)) return i;
  }
  return -1;
}

// Which card comes next and how long it stands. A sequence decides both;
// without one it is the next filled slot for SLIDE_HOLD_MS.
static bool slideNext(int *card, uint32_t *ms) {
  const int n = cfgSeqCount();
  if (n > 0) {
    // Steps over entries whose card is not available (a PCM card with
    // no ADC attached): the show goes on with the rest, and gives up
    // only when nothing in the sequence is available.
    for (int tries = 0; tries < n; tries++) {
      if (slideSeqIndex < 0 || slideSeqIndex >= n - 1) {
        slideSeqIndex = 0;
      } else {
        slideSeqIndex++;
      }
      const int c = cfgSeqCard(slideSeqIndex);
      if (c < 0 || c >= PATTERN_COUNT || !cardIsAvailable(c)) continue;
      *card = c;
      *ms = cfgSeqMs(slideSeqIndex);
      return true;
    }
    return false;
  }
  const int next = nextFilledCustom(patternIndex);
  if (next < 0) return false;
  *card = next;
  *ms = SLIDE_HOLD_MS;
  return true;
}

// `remember` off is for the case where a card is being chosen by hand:
// that choice is written a moment later anyway, and two erases in a row
// is one too many.
static void slideshowSet(bool on, bool remember = true) {
  if (on == slideshowOn) return;

  if (on) {
    if (cfgSeqCount() == 0 && nextFilledCustom(patternIndex) < 0) {
      report(LANG_SLIDE_NO_SEQUENCE_AND_NO);
      // RAM flag only; flash follows on the next card write.
      cfgSetSlideshowOn(false);
      return;
    }
    slideshowOn = true;
    slidePhase = SLIDE_SHOWING;
    slideStep = 0;
    slideSeqIndex = -1;          // so the first step is step one
    slideHoldUntil = millis();   // and the first change comes straight away
    cfgSetSlideshowOn(true);
    saveCardNow();
    if (cfgSeqCount() > 0) {
      report(LANG_SLIDE_ON_D_STEPS, cfgSeqCount());
    } else {
      report(LANG_SLIDE_ON_EVERY_FILLED_SLOT, (unsigned long)SLIDE_HOLD_MS);
    }
    return;
  }

  slideshowOn = false;
  slidePhase = SLIDE_SHOWING;
  slideStep = 0;
  advSetFade(256);   // whatever it was in the middle of, back to full
  cfgSetSlideshowOn(false);

  if (remember) {
    cfgSetStartPattern(patternIndex);
    saveCardNow();
  }
  report(LANG_SLIDE_OFF);
}

// One step, once per picture. Called from videoFrameHook().
static void slideshowStep() {
  switch (slidePhase) {
    case SLIDE_SHOWING:
      if ((int32_t)(millis() - slideHoldUntil) < 0) return;
      slidePhase = SLIDE_OUT;
      slideStep = 0;
      return;

    case SLIDE_OUT:
      // Off skips the ramp, not the step: still one advSetFade()
      // straight to black and one picture on black before the switch.
      if (!cfgSlideFadeOn()) {
        advSetFade(0);
        slidePhase = SLIDE_BLACK;
        slideWaited = 0;
        slideSwitched = false;
        return;
      }
      slideStep++;
      advSetFade(256 * (SLIDE_FADE_OUT_STEPS - slideStep) / SLIDE_FADE_OUT_STEPS);
      if (slideStep >= SLIDE_FADE_OUT_STEPS) {
        slidePhase = SLIDE_BLACK;
        slideWaited = 0;
        slideSwitched = false;
      }
      return;

    case SLIDE_BLACK:
      // Waits one picture after the last fade step: the scale registers
      // are double buffered, so switching in the same breath would put
      // the new picture up for one field at the fade's last level.
      if (!slideSwitched) {
        slideSwitched = true;
        int next = 0;
        uint32_t ms = SLIDE_HOLD_MS;
        if (slideNext(&next, &ms)) {
          slideCurrentMs = ms;
          selectPattern(next);
        }
        return;
      }

      // Waits until the new card is really delivering rows, or the
      // blank catch-up rows would show.
      if (switchRunning && ++slideWaited < SLIDE_WAIT_MAX) return;
      slidePhase = SLIDE_IN;
      slideStep = SLIDE_FADE_IN_STEPS;
      return;

    case SLIDE_IN:
      // Same choice as SLIDE_OUT: off cuts straight to full.
      if (!cfgSlideFadeOn()) {
        advSetFade(256);
        slidePhase = SLIDE_SHOWING;
        slideHoldUntil = millis() + slideCurrentMs;
        return;
      }
      slideStep--;
      advSetFade(256 * (SLIDE_FADE_IN_STEPS - slideStep) / SLIDE_FADE_IN_STEPS);
      if (slideStep <= 0) {
        slidePhase = SLIDE_SHOWING;
        slideHoldUntil = millis() + slideCurrentMs;
      }
      return;
  }
}

static void printPatterns() {
  for (int i = 0; i < PATTERN_COUNT; i++) {
    // An empty slot is worth marking: p and P walk past it, and only
    // typing its number gets you there, to upload into.
    const int slot = customSlotOf(PATTERNS[i]);
    const char *file = customFileName(PATTERNS[i]);
    const char *tail = (slot < 0) ? "" : (file ? file : LANG_EMPTY_SKIPPED_BY_P);
    Serial.printf(LANG_2D_S_S_S, i + 1, (i == patternIndex) ? "*" : " ",
                  PATTERNS[i]->name, tail[0] ? "  [" : "", tail, tail[0] ? "]" : "");
  }
}

static void printHelp() {
  Serial.println(LANG_HELP_KEYS);
  Serial.println(LANG_P_P_NEXT_PREVIOUS);
  Serial.println(LANG_W_ASPECT_RATIO_AND);
  Serial.println(LANG_I_INSERT_BOXES_NONE);
  Serial.println(LANG_1_5_LUMA_FILTER);
  Serial.println(LANG_X_CHROMA_FILTER);
  Serial.println(LANG_N_TEXT_IN_THE);
  Serial.println(LANG_S_TEXT_IN_THE);
  Serial.println(LANG_F_FONT_PM5544_PM8546);
  Serial.println(LANG_B_G924_CHROMA_TEST);
  Serial.println(LANG_D_TIMING_REPORT_ON);
  Serial.println(LANG_K_SHOW_THE_CLOCK);
  Serial.println(LANG_T_SET_THE_CLOCK);
  Serial.println(LANG_R_WRITE_THE_ENCODER);
  Serial.println(LANG_R_HARDWARE_RESET_OF);
  Serial.println(LANG_Z_SFL_TIMING_RESET);
  Serial.println(LANG_V_VBI_OPEN_CLOSED);
  Serial.println(LANG_E_TELETEXT_ON_OFF);
  Serial.println(LANG_I_INSERTION_TEST_SIGNALS);
  Serial.println(LANG_A_PICK_AN_ANALOGUE);
  Serial.println(LANG_U_UPLOAD_A_PHOTOGRAPH);
  Serial.println(LANG_H_THE_FIRST_TEXT);
  Serial.println(LANG_SEVEN_CARDS_HAVE_NO);
  Serial.println(LANG_Q_THE_SECOND_TEXT);
  Serial.println(LANG_M_THE_MOVING_LINE);
  Serial.println(LANG_ONLY_ON_A_CARD);
  Serial.println(LANG_N_THE_TICKER_ON);
  Serial.println(LANG_W_THE_TICKER_TEXT);
  Serial.println(LANG_Y_Y_THE_TICKER);
  Serial.println(LANG_Z_THE_TICKER_FONT);
  Serial.println(LANG_S_SLIDESHOW_OF_THE);
  Serial.println(LANG_Q_THE_SLIDESHOW_SEQUENCE);
  Serial.println(LANG_L_LIST_THE_SIXTEEN);
  Serial.println(LANG_D_READ_A_SLOT);
  Serial.println(LANG_X_SEND_A_CARD);
  Serial.println(LANG_C_SEND_ONE_OF);
  Serial.println(LANG_E_EMPTY_A_SLOT);
  Serial.println(LANG_U_SHOW_WHAT_IS);
  Serial.println(LANG_C_CONTEST_SET_THE);
  Serial.println(LANG_A_EVERY_SETTING_AS);
  Serial.println(LANG_K_SET_ONE_NAME);
  Serial.println(LANG_WRITE_THE_SETTINGS_TO);
  Serial.println(LANG_NOTHING_ELSE_DOES_SO);
  Serial.println(LANG_WHICH_CARD_IS_ON);
  Serial.println(LANG_F_SETTINGS_BACK_TO);
  Serial.println(LANG_G_REBOOT_INTO_THE);
  Serial.println(LANG_L_DISPLAY_ALIGNMENT_PICTURE);
  Serial.println(LANG_O_O_J_J);
  Serial.println(LANG_H_G_V_B);
  Serial.println(LANG_OTHER_WAY_ROUND);
  Serial.println(LANG_THIS_LIST);
}

// Requests an SFL timing reset; checked later by sflFinish(). Must not
// wait here (runs on core0's producer loop, videoFrameHook()): waiting
// for the pulse would starve the ring and hang core1's streamer, which
// waits on rows core0 would never produce. So this only requests the
// pulse; the frame loop finishes it.
static bool sflBusy = false;
static uint32_t sflRequestedMs = 0;
static uint8_t sflFieldBefore = 0;

static void sflTimingReset() {
  if (sflBusy) return;
  sflFieldBefore = advReadFieldCount();
  advSetTimingReset(true);
  videoRequestSflPulse();
  sflBusy = true;
  sflRequestedMs = millis();
  report(LANG_SFL_PULSE_REQUESTED_TO_BE);
}

// Checked at every frame. The streamer runs tens of rows behind the
// producer, so it takes at most a picture or two.
static void sflFinish() {
  if (!sflBusy) return;
  if (videoSflPulseBusy()) {
    if (millis() - sflRequestedMs > 500) {
      sflBusy = false;
      // Abort AND put the pin low, or it stays high and the encoder
      // holds its counters in reset, blanking the picture for good.
      videoAbortSflPulse();
      advSetTimingReset(false);
      report(LANG_SFL_PULSE_NOT_CARRIED_OUT);
    }
    return;
  }
  sflBusy = false;
  // Armed only around the pulse: left armed, GP18 is an edge sensitive
  // reset pin next to the 27 MHz clock, where a glitch blanks the picture.
  advSetTimingReset(false);
  report(LANG_SFL_TIMING_RESET_DONE_FIELD,
       (unsigned)sflFieldBefore, (unsigned)advReadFieldCount());
}

// Photograph upload. See photoflash.h for the flash layout, only the
// serial protocol is here.
//
// Takes over the loop: videoFrameHook() normally reads 32 characters a
// picture (800 byte/s), far too slow for an 810 kB photograph, so
// during an upload core0 sits here instead of in the render loop, and
// core1 is parked per sector so it can be written to.
//
// Flow control is compulsory, not a courtesy: writing turns interrupts
// off, so an unacknowledged sender would overflow the unserviced USB.
//
// PROTOCOL, everything in blocks of PHOTO_SECTOR bytes:
//   PC  -> "u"                      (the key that opens this mode)
//   Pico-> "[upload] ready <bytes> <sectors>"
//   PC  -> "<slot> <name>" on one line, slot is 1 through PHOTO_SLOTS
//   Pico-> "[upload] erasing"  ... "[upload] erased"
//   PC  -> sector 0 (4096 bytes)
//   Pico-> "A<n>"                   (one acknowledgement per sector)
//   ...  until every sector has arrived
//   Pico-> "[upload] done, crc 0x........" or an error message
//
// These messages are protocol, not prose: both tools search on this
// text (PC Software/src/BoardUpload.cs, tools/convert_image.py),
// including the error words failed, aborted, too small and does not
// exist.
//
// Cancelled with Escape as long as no data is flowing; times out on
// silence, see UPLOAD_TIMEOUT_MS.
static const uint32_t UPLOAD_TIMEOUT_MS = 5000;

static bool readExactly(uint8_t *buf, uint32_t len) {
  uint32_t n = 0;
  uint32_t t0 = millis();
  while (n < len) {
    int c = Serial.read();
    if (c < 0) {
      if (millis() - t0 > UPLOAD_TIMEOUT_MS) return false;
      // The 5 s timeout bounds the gap between bytes, not the whole
      // wait, so a slow sender could outlast the 8 s watchdog. Video
      // runs during these waits, so the feed is genuine.
      videoFeedWatchdog();
      continue;
    }
    buf[n++] = (uint8_t)c;
    t0 = millis();
  }
  return true;
}

// Sets the status panel's busy text and pushes it out right now,
// blocking: displayFrameHook()/displayStep() normally spread a
// full-panel transfer over several pictures from loop(), but
// photoEraseSlot() and photoUpload() run inside a single blocking call
// with the video suspended and never return to loop() until done, so
// nothing would otherwise pump it. Four rounds, not one: the first
// picks up and drains a candidate/menu transfer the knob left mid
// flight (displayFrameHook() only queues the busy text once nothing
// else is going out), the second queues and drains the busy text
// itself, the rest are free once nothing is left to send. Each
// displayStep() budget is one byte bigger than the whole panel, so a
// queued transfer always finishes in that one call.
static void displayBusyNow(const char *text) {
  displaySetBusy(text);
  const unsigned wholePanel = (unsigned)(DISP_W * DISP_H * 2) + 1;
  for (int tries = 0; tries < 4; tries++) {
    displayFrameHook();
    displayStep(wholePanel);
  }
}

// Getting everything going again after an upload, without a restart:
// 1. flashSpeedApply() -- the SDK's flash routines rerun boot2, which
//    resets the flash clock divisor; needed back or a photograph
//    stutters reading 1440 bytes a row straight out of flash.
// 2. videoResume() -- puts the producer back on the streamer's line
//    counter.
// 3. cfgReload() -- reruns the active card's init, since custom.cpp
//    only checks a slot's validity there.
static void uploadFinish() {
  flashSpeedApply();
  videoResume();
  cfgReload();
  displayBusyNow(nullptr);
}

// --- reading a slot back, and emptying one ---------------------------
//
// The counterpart of photoUpload(); same shape and words as
// PC Software/src/BoardSlots.cs.
//
// PROTOCOL, the list:
//   PC  -> "l"
//   Pico-> "[slots] <count> <bytes> <width> <height>"
//   Pico-> "[slot] <n> filled 0x<crc> <name>"   for every filled slot
//   Pico-> "[slot] <n> empty"                   for every empty one
//   Pico-> "[slots] end"
//
// PROTOCOL, reading one back:
//   PC  -> "D"
//   Pico-> "[download] ready, send the slot number"
//   PC  -> "<slot>" on one line
//   Pico-> "[download] slot <n>, <bytes> bytes, crc 0x........"
//   Pico-> the bytes, raw, exactly that many
//   Pico-> "[download] done"
//
// PROTOCOL, emptying one:
//   PC  -> "E"
//   Pico-> "[erase] ready, send the slot number"
//   PC  -> "<slot>" on one line
//   Pico-> "[erase] erasing slot <n> (the picture goes black meanwhile)"
//   Pico-> "[erase] done, slot <n> is empty"
//
// Protocol, not prose, exactly as photoUpload(): a word changed here
// changes BoardSlots.cs with it, the same four failure words included.

// Hooks the producer back on the streamer's line counter if typing
// delay let it fall behind; skipped unless it actually lags.
static void resyncIfBehind() {
  if ((int32_t)(videoProducedCount - videoConsumedCount) < 0) videoResume();
}

static int readNumber(const char *what, int most) {
  char line[16];
  int n = 0;
  uint32_t t0 = millis();
  for (;;) {
    int c = Serial.read();
    if (c < 0) {
      if (millis() - t0 > UPLOAD_TIMEOUT_MS) {
        Serial.printf(LANG_S_ABORTED_NO_NUMBER, what);
        resyncIfBehind();
        return -1;
      }
      videoFeedWatchdog();   // see readExactly(): the timeout is per byte
      continue;
    }
    if (c == 27) {
      Serial.printf(LANG_S_ABORTED, what);
      resyncIfBehind();
      return -1;
    }
    if (c == 13) continue;
    if (c == 10) break;
    if (n < (int)sizeof(line) - 1) line[n++] = (char)c;
    t0 = millis();
  }
  line[n] = 0;

  int v = 0;
  const char *p = line;
  while (*p == ' ') p++;
  if (*p < '0' || *p > '9') {
    Serial.printf(LANG_S_ABORTED_S_IS, what, line);
    resyncIfBehind();
    return -1;
  }
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  resyncIfBehind();   // the typing itself already cost the producer time
  if (v < 1 || v > most) {
    Serial.printf(LANG_S_D_DOES_NOT, what, v, most);
    return -1;   // resyncIfBehind() ran just above
  }
  return v - 1;
}

// What is in the slots, one line per slot with the fields always in
// the same place, for a program to read; key U prints the same for a
// human.
static void photoListSlots() {
  Serial.printf(LANG_SLOTS_D_LU_D_D, PHOTO_SLOTS, (unsigned long)PHOTO_BYTES,
                (int)SCREEN_W, (int)SCREEN_H);
  for (int i = 0; i < PHOTO_SLOTS; i++) {
    if (photoValid(i)) {
      const PhotoHeader *h = photoHeader(i);
      Serial.printf(LANG_SLOT_D_FILLED_0X_08LX, i + 1,
                    (unsigned long)h->crc, h->name[0] ? h->name : LANG_NO_NAME);
    } else {
      Serial.printf(LANG_SLOT_D_EMPTY, i + 1);
    }
  }
  // The sequence goes with the listing, so the PC tool has both from
  // one exchange rather than a separate command.
  {
    char now[SEQ_TEXT_MAX];
    cfgSeqText(now, sizeof(now));
    Serial.print(LANG_SEQ_NOW);
    Serial.println(now[0] ? now : LANG_NONE);
  }
  Serial.println(LANG_SLOTS_END);
}

// Sends a slot back over the port, raw. No picture meanwhile (core0
// builds no rows, the streamer sends black with correct sync), and no
// flashSpeedApply() needed since this only reads XIP. videoResume() at
// the end re-syncs the producer with the streamer's line counter, which
// kept running; safe to call without videoSuspend() first.
static void photoDownload() {
  if (!Serial) return;
  Serial.println(LANG_DOWNLOAD_READY_SEND_THE_SLOT);
  const int slot = readNumber(LANG_DOWNLOAD, PHOTO_SLOTS);
  if (slot < 0) return;

  if (!photoValid(slot)) {
    Serial.printf(LANG_DOWNLOAD_SLOT_D_IS_EMPTY, slot + 1);
    return;
  }

  const PhotoHeader *h = photoHeader(slot);
  Serial.printf(LANG_DOWNLOAD_SLOT_D_LU_BYTES, slot + 1,
                (unsigned long)PHOTO_BYTES, (unsigned long)h->crc, h->name);

  // In pieces, with the count checked: SerialUSB::write() gives up
  // after a second without progress, so a host that stops reading shows
  // up as an abort here, not as a picture quietly half sent.
  const uint8_t *src = photoData(slot);
  uint32_t sent = 0;
  while (sent < PHOTO_BYTES) {
    // Can run the whole download without loop() seeing its top again;
    // same reasoning as the upload sector loop.
    videoFeedWatchdog();
    const uint32_t want = (PHOTO_BYTES - sent > PHOTO_SECTOR) ? PHOTO_SECTOR : PHOTO_BYTES - sent;
    const size_t got = Serial.write(src + sent, want);
    if (got == 0) {
      videoResume();
      Serial.printf(LANG_DOWNLOAD_ABORTED_AT_LU_BYTES,
                    (unsigned long)sent);
      return;
    }
    sent += (uint32_t)got;
  }
  videoResume();
  Serial.println(LANG_DOWNLOAD_DONE);
}

// Empties a slot: erase it and do NOT write a header afterwards, so
// photoValid() says no from here on. That is the whole difference with an
// upload, which finishes with photoFinish().
static void photoEraseSlot() {
  if (!Serial) return;
  Serial.println(LANG_ERASE_READY_SEND_THE_SLOT);
  const int slot = readNumber(LANG_ERASE, PHOTO_SLOTS);
  if (slot < 0) return;

  if (!photoHeaderOk(slot)) {
    Serial.printf(LANG_ERASE_SLOT_D_WAS_ALREADY, slot + 1);
    return;
  }

  // Core1 into its RAM loop before a single byte of flash is touched;
  // the same rule as at photoUpload().
  if (!videoSuspend()) {
    Serial.println(LANG_ERASE_ABORTED_CORE1_DID_NOT);
    return;
  }
  Serial.printf(LANG_ERASE_ERASING_SLOT_D_THE, slot + 1);

  char busyText[40];
  snprintf(busyText, sizeof(busyText), LANG_ERASING_SLOT_D, slot + 1);
  displayBusyNow(busyText);

  const bool ok = photoBeginWrite(slot);
  uploadFinish();

  if (ok) {
    Serial.printf(LANG_ERASE_DONE_SLOT_D_IS, slot + 1);
  } else {
    Serial.printf(LANG_ERASE_FAILED_ON_SLOT_D, slot + 1);
  }
}

// Shared by photoUpload() and portraitUploadFlow(), never both active
// at once: one static 4 kB block instead of two, RAM is tight.
static uint8_t uploadBlock[PHOTO_SECTOR];

static void photoUpload() {
  if (!Serial) return;
  // 829440 / 4096 = 202.5, so the last sector is only half full. It is
  // padded with zeroes; the CRC runs over PHOTO_BYTES and not over the
  // sector, so that padding does not count.
  uint32_t rest = PHOTO_BYTES % PHOTO_SECTOR;
  uint32_t total = PHOTO_BYTES / PHOTO_SECTOR + (rest ? 1 : 0);

  if (photoSlotSpace() < PHOTO_BYTES) {
    Serial.printf(LANG_UPLOAD_SLOT_TOO_SMALL_LU,
                  (unsigned long)photoSlotSpace(), (unsigned long)PHOTO_BYTES);
    return;
  }

  Serial.printf(LANG_UPLOAD_READY_LU_LU, (unsigned long)PHOTO_BYTES, (unsigned long)total);
  Serial.println(LANG_UPLOAD_SEND_THE_NAME_ON);

  char name[33];
  int n = 0;
  uint32_t t0 = millis();
  for (;;) {
    int c = Serial.read();
    if (c < 0) {
      if (millis() - t0 > UPLOAD_TIMEOUT_MS) {
        Serial.println(LANG_UPLOAD_ABORTED_NO_NAME_RECEIVED);
        resyncIfBehind();
        return;
      }
      videoFeedWatchdog();   // see readExactly(): the timeout is per byte
      continue;
    }
    if (c == 27) {
      Serial.println(LANG_UPLOAD_ABORTED);
      resyncIfBehind();   // the only abort that skipped it; typing can leave minutes of backlog
      return;
    }
    if (c == 13) continue;
    if (c == 10) break;
    if (n < (int)sizeof(name) - 1) name[n++] = (char)c;
    t0 = millis();
  }
  name[n] = 0;

  // The line starts with the slot number: "1 photo.jpg" through
  // "16 photo.jpg". Read as multiple digits, or "16" would parse as
  // slot 1 with a file called "6 photo.jpg". No number means slot 1.
  int slot = 0;
  char *pn = name;
  if (pn[0] >= '0' && pn[0] <= '9') {
    int n = 0;
    while (*pn >= '0' && *pn <= '9') n = n * 10 + (*pn++ - '0');
    if (n >= 1 && n <= PHOTO_SLOTS) {
      slot = n - 1;
      while (*pn == ' ') pn++;
    } else {
      Serial.printf(LANG_UPLOAD_SLOT_D_DOES_NOT, n, PHOTO_SLOTS);
      uploadFinish();
      return;
    }
  }
  Serial.printf(LANG_UPLOAD_SLOT_D_CUSTOM_D, slot + 1, slot + 1, pn);

  // Core1 into its RAM loop once, around the whole run of sectors: it
  // keeps sending black rows with correct EAV/SAV so CLKIN and the line
  // structure never stall, while core0 sits here building none.
  if (!videoSuspend()) {
    Serial.println(LANG_UPLOAD_ABORTED_CORE1_DID_NOT);
    return;
  }

  char busyText[40];
  snprintf(busyText, sizeof(busyText), LANG_UPLOADING_S, pn);
  displayBusyNow(busyText);

  Serial.println(LANG_UPLOAD_ERASING_THIS_TAKES_A);
  if (!photoBeginWrite(slot)) {
    uploadFinish();
    Serial.println(LANG_UPLOAD_ERASE_FAILED);
    return;
  }
  Serial.println(LANG_UPLOAD_ERASED);

  for (uint32_t i = 0; i < total; i++) {
    // A full upload runs this loop without returning to loop()'s top
    // until done or failed, so fed once a sector to keep the watchdog
    // fed; see videoFeedWatchdog().
    videoFeedWatchdog();
    memset(uploadBlock, 0, sizeof(uploadBlock));  // the last sector is only partly filled
    uint32_t wil = (i + 1 == total && rest) ? rest : PHOTO_SECTOR;
    if (!readExactly(uploadBlock, wil)) {
      Serial.printf(LANG_UPLOAD_ABORTED_AT_SECTOR_LU, (unsigned long)i);
      uploadFinish();
      Serial.println(LANG_UPLOAD_SLOT_STAYS_INVALID_THE);
      return;
    }
    if (!photoWriteSector(slot, i, uploadBlock)) {
      Serial.printf(LANG_UPLOAD_WRITE_FAILED_AT_SECTOR, (unsigned long)i);
      uploadFinish();
      Serial.println(LANG_UPLOAD_SLOT_STAYS_INVALID_THE);
      return;
    }
    Serial.printf(LANG_A_LU, (unsigned long)i);
  }

  uint32_t crc = photoCrcFromFlash(slot);
  bool ok = photoFinish(slot, pn, crc);

  uploadFinish();
  // Written out, not assembled with %s: both tools wait for the literal
  // text "[upload] done," and the word "failed".
  if (ok) {
    Serial.printf(LANG_UPLOAD_DONE_CRC_0X_08LX,
                  (unsigned long)crc, pn, flashSpeedDiv());
  } else {
    Serial.printf(LANG_UPLOAD_HEADER_WRITE_FAILED_SLOT,
                  (unsigned long)crc);
  }
}

// The BBC Test Card F/W photo replacements, triggered over the "K"
// generic-setting line as "portrait.0=1"/"portrait.1=1" (see
// setSettingFromCommand()), not the 'u' key: photoUpload() announces
// its byte count (PHOTO_BYTES, fixed) before it learns which slot the
// name line asks for, which only works because every Custom slot is
// the same size. F and W are not the same size as each other, so this
// has to know its target, and announce that target's own byte count,
// before anything else.
static void portraitUploadFlow(int target) {
  if (!Serial) return;
  if (target < 0 || target >= PORTRAIT_COUNT) {
    Serial.println(LANG_CFG_FAILED_NO_SUCH_CARD);
    return;
  }

  const uint32_t bytes = portraitBytes(target);
  const uint32_t rest = bytes % PHOTO_SECTOR;
  const uint32_t total = bytes / PHOTO_SECTOR + (rest ? 1 : 0);
  Serial.printf(LANG_UPLOAD_READY_LU_LU, (unsigned long)bytes, (unsigned long)total);

  if (!videoSuspend()) {
    Serial.println(LANG_UPLOAD_ABORTED_CORE1_DID_NOT);
    return;
  }

  char busyText[40];
  snprintf(busyText, sizeof(busyText), LANG_UPLOADING_S,
           target == PORTRAIT_TESTCARDF ? "F photo" : "W photo");
  displayBusyNow(busyText);

  if (!portraitBeginWrite(target)) {
    uploadFinish();
    Serial.println(LANG_UPLOAD_ERASE_FAILED);
    return;
  }
  Serial.println(LANG_UPLOAD_ERASED);

  for (uint32_t i = 0; i < total; i++) {
    videoFeedWatchdog();
    memset(uploadBlock, 0, sizeof(uploadBlock));  // the last sector is only partly filled
    uint32_t wil = (i + 1 == total && rest) ? rest : PHOTO_SECTOR;
    if (!readExactly(uploadBlock, wil)) {
      Serial.printf(LANG_UPLOAD_ABORTED_AT_SECTOR_LU, (unsigned long)i);
      uploadFinish();
      Serial.println(LANG_UPLOAD_SLOT_STAYS_INVALID_THE);
      return;
    }
    if (!portraitWriteSector(target, i, uploadBlock)) {
      Serial.printf(LANG_UPLOAD_WRITE_FAILED_AT_SECTOR, (unsigned long)i);
      uploadFinish();
      Serial.println(LANG_UPLOAD_SLOT_STAYS_INVALID_THE);
      return;
    }
    Serial.printf(LANG_A_LU, (unsigned long)i);
  }

  const uint32_t crc = portraitCrcFromFlash(target);
  const bool ok = portraitFinish(target, crc);

  uploadFinish();
  if (ok) {
    Serial.printf(LANG_UPLOAD_DONE_CRC_0X_08LX, (unsigned long)crc,
                  target == PORTRAIT_TESTCARDF ? "F photo" : "W photo", flashSpeedDiv());
  } else {
    Serial.printf(LANG_UPLOAD_HEADER_WRITE_FAILED_SLOT, (unsigned long)crc);
  }
}

static void printPhotoSlots() {
  for (int i = 0; i < PHOTO_SLOTS; i++) {
    if (!photoValid(i)) {
      Serial.printf(LANG_PHOTO_SLOT_D_CUSTOM_D, i + 1, i + 1);
      continue;
    }
    const PhotoHeader *k = photoHeader(i);
    Serial.printf(LANG_PHOTO_SLOT_D_CUSTOM_D_2, i + 1, i + 1,
                  k->name, k->width, k->height, (unsigned long)k->crc);
  }
}

static void printParam() {
  char buf[120];
  advParamText(buf, sizeof(buf));
  Serial.printf(LANG_CONTROL_D_D_S, advParamIndex() + 1, advParamCount(), buf);
}

static void printVbi() {
  Serial.printf(LANG_VBI_VERTICAL_BLANKING_S,
                advVbiOpen() ? LANG_VBI_OPEN
                             : LANG_VBI_CLOSED);
}

static void printTeletext() {
  char b[220];
  teletextStatus(b, sizeof(b));
  Serial.printf(LANG_TELETEXT_S, b);
  if (teletextOn() && teletextPacketsPerField() < TT_PACKETS) {
    Serial.printf(LANG_ITS_TAKES_LINES_SO,
                  teletextPacketsPerField(), TT_PACKETS,
                  TT_PACKETS - teletextPacketsPerField());
  }
}

static void printIts() {
  char b[120];
  itsStatus(b, sizeof(b));
  Serial.printf(LANG_ITS_S, b);
}

static void printInserts() {
  Serial.printf(LANG_INSERT_S, insertModeName(insertGetMode()));
}

static void printChromaFilter() {
  int i = advChromaFilterIndex();
  Serial.printf(LANG_CHROMA_D_D_S, i + 1, advChromaFilterCount(), advChromaFilterName(i));
}

static void printAspect() {
  Serial.printf(LANG_ASPECT_D_D_S, aspectGet() + 1, ASPECT_COUNT, aspectName(aspectGet()));
}

static void printLumaFilter() {
  int i = advLumaFilterIndex();
  Serial.printf(LANG_FILTER_D_0X_02X_S, i + 1, advLumaFilterReg(i), advLumaFilterName(i));
}

// Sets the date and/or the time by reading every number out of the
// line, so both "T2026-08-09 14:30:00" and "T2026/08/09 14.30.00" work.
static void setClockFromCommand(const char *s) {
  int v[6], n = 0;
  for (const char *p = s; *p && n < 6;) {
    if (*p >= '0' && *p <= '9') {
      int x = 0;
      while (*p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
      v[n++] = x;
    } else {
      p++;
    }
  }

  // Three numbers are ambiguous: 09-08-26 could be a date and 12:30:45
  // a time. A first number above 99 can only be a year, so that is what
  // tells them apart.
  bool hasDate = (n >= 5) || (n == 3 && v[0] > 99);
  bool hasTime = (n >= 5) || (n == 2) || (n == 3 && v[0] <= 99);

  if (!hasDate && !hasTime) {
    Serial.println(LANG_CLOCK_USAGE_TYYYY_MM_DD);
    return;
  }

  ClockTime t;
  clockGet(&t);
  int year = t.year, month = t.month, day = t.day;
  int hour = t.hour, min = t.min, sec = t.sec;

  if (hasDate) {
    year = v[0];
    month = v[1];
    day = v[2];
    if (year < 100) year += 2000;
  }
  if (hasTime) {
    int o = hasDate ? 3 : 0;
    hour = v[o];
    min = v[o + 1];
    sec = (n - o >= 3) ? v[o + 2] : 0;
  }

  if (year < 1970 || year > 2999 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour > 23 || min > 59 || sec > 59) {
    Serial.println(LANG_CLOCK_VALUE_OUT_OF_RANGE);
    return;
  }

  clockSet(year, month, day, hour, min, sec);
  Serial.printf(LANG_CLOCK_SET_TO_04D_02D, year, month, day, hour, min,
                sec);
}

// Text entry for the T command: while it runs, all characters go to the
// line buffer, so single key commands stop working until a message,
// Escape or a timeout ends it. Sized for a whole slideshow sequence
// ("sequence=" plus up to SEQ_TEXT_MAX), the longest thing that comes
// in this way. cmdOverflow refuses a line that does not fit rather
// than silently using it short.
static char cmdLine[SEQ_TEXT_MAX + 48];
static int cmdLen = -1;  // -1 = no line in progress
static bool cmdOverflow = false;
static char cmdKind = 0;
static uint32_t cmdLastMs = 0;
static const uint32_t CMD_TIMEOUT_MS = 5000;

static void cmdAbort(const char *why) {
  cmdLen = -1;
  cmdOverflow = false;
  Serial.printf(LANG_CLOCK_INPUT_S_THE_KEYS, why);
}

// Records a changed setting without stopping the video chain: core1's
// streamer keeps sending black rows with correct EAV/SAV while core0
// erases and programs, so nothing needs restoring afterwards. Returns
// whether it really wrote, since a write empties the ring buffer and
// the caller must not judge the producer in the same breath. Only the
// current card, not the rest of the settings; cfgSaveCard() does that.
static bool saveCardNow() {
  if (!cfgCardDirty()) return false;
  if (!cfgSaveCard()) {
    Serial.println(LANG_STORAGE_SAVING_THE_CARD_FAILED);
    return false;
  }
  return true;
}

static bool saveNow() {
  if (!cfgDirty()) return false;
  if (!cfgSave()) {
    Serial.println(LANG_STORAGE_SAVING_FAILED_NOTHING_IN);
    return false;
  }
  // QSPI divisor reported too, since the SDK's flash routines reset it
  // and it is restored right after; the photographs depend on it.
  // A normal save only programs the journal, a thin flicker at most; a
  // ring that ran dry mid-session erases instead and blacks the
  // picture visibly, said out loud here so it explains itself.
  Serial.printf(LANG_STORAGE_SAVED_S_QSPI_DIVISOR,
                cfgLastSaveErased() ? LANG_JOURNAL_RING_WAS_FULL : "",
                flashSpeedDiv());
  return true;
}

// Takes the sequence off the serial port. Words are protocol:
// PC Software/src/BoardSequence.cs waits for "[seq] set", "[seq]
// cleared", and recognises trouble by "failed".
static void setSequenceFromCommand(const char *line) {
  char why[96];
  const int n = cfgSeqParse(line, PATTERN_COUNT, why, sizeof(why));
  if (n < 0) {
    Serial.printf(LANG_SEQ_FAILED_S, why);
    return;
  }
  if (n == 0) {
    Serial.println(LANG_SEQ_CLEARED_THE_SLIDESHOW_TAKES);
    return;
  }

  char now[SEQ_TEXT_MAX];
  cfgSeqText(now, sizeof(now));
  Serial.print(LANG_SEQ_SET);
  Serial.println(now);

  // A step may name a photograph slot that is empty. That is not an
  // error: the slot can be filled later, and refusing it would mean
  // having to type the sequence again afterwards. It is worth saying so
  // though, because such a step shows a flat grey.
  for (int i = 0; i < n; i++) {
    const int card = cfgSeqCard(i);
    const int slot = customSlotOf(PATTERNS[card]);
    if (slot >= 0 && !photoValid(slot)) {
      Serial.printf(LANG_SEQ_NOTE_STEP_D_IS,
                    i + 1, card + 1);
    }
  }
}

// --- the settings over the port, for the PC tool ----------------------
//
// One dump and one set line is the whole protocol: key A writes
// everything out, key K reads one setting back as "name=value", so a
// setting added later needs no new key or command, only a known name.
//
// A card scoped name carries its number: card.7.tickery=530, counted
// from 1 like the list under ?. Everything else is shared.
//
// Built with snprintf into our own buffer rather than Serial.printf, to
// avoid its stack/heap split for long lines.
static void cfgLine(const char *fmt, ...) {
  // Sized well past the longest line: 240 was once outgrown by the
  // [card] line and truncated it silently, gluing the name onto the cut
  // CRC.
  char msg[400];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (n > 0) Serial.write(msg, n < (int)sizeof(msg) ? n : (int)sizeof(msg) - 1);
}

// A text value can be as long as the ticker message, which is past what
// one printf line should carry, so those go out in two pieces.
static void cfgLineText(const char *name, const char *value) {
  Serial.print(LANG_CFG);
  Serial.print(name);
  Serial.print(' ');
  Serial.println(value);
}

// What the PC keeps a picture under: cfgCardCrc() only covers what the
// settings change about a card, so the slot's own checksum is mixed in
// here too. Used by the dump and by a card-scoped set's answer, which
// must agree.
static uint32_t cardCrcForPc(int i) {
  uint32_t crc = cfgCardCrc(i);
  const int slot = customSlotOf(PATTERNS[i]);
  if (slot >= 0 && photoValid(slot)) crc ^= photoHeader(slot)->crc;
  return crc;
}

static void printSettings() {
  // Protocol version comes FIRST: a mismatched tool/board pair would
  // otherwise misread the reply and hang with no clue why. Bump it
  // whenever the shape of anything below changes.
  Serial.println(LANG_CFG_BEGIN);
  cfgLine(LANG_CFG_PROTO_D, 26);
  cfgLine(LANG_CFG_CARDS_D, PATTERN_COUNT);
  cfgLine(LANG_CFG_ASPECTS_D, ASPECT_COUNT);
  cfgLine(LANG_CFG_INSERTS_D, INSERT_COUNT);
  cfgLine(LANG_CFG_FONTS_D, FONT_COUNT);
  cfgLine(LANG_CFG_SLOTS_D, PHOTO_SLOTS);
  cfgLine(LANG_CFG_TELETEXTROWS_D, TT_ROWS);

  cfgLineText(LANG_TEXTID, cfgTextId());
  cfgLineText(LANG_TEXTSUB, cfgTextSub());
  cfgLineText(LANG_TICKERTEXT, cfgTickerText());
  cfgLineText(LANG_CONTEST1, cfgContestText1());
  cfgLineText(LANG_CONTEST2, cfgContestText2());
  cfgLineText(LANG_PCMTEXT, cfgPcmText());
  cfgLineText(LANG_INSERTLEFT, cfgInsertLeft());
  cfgLineText(LANG_INSERTRIGHT, cfgInsertRight());
  for (int r = 1; r <= TT_ROWS; r++) {
    char nm[16];
    snprintf(nm, sizeof(nm), LANG_TELETEXT_D, r);
    // The real current page, not the raw (possibly empty) setting: the
    // PC tool's editor opens on what is actually on screen.
    char txt[TT_COLS + 1];
    teletextRowText(r, txt, sizeof(txt));
    cfgLineText(nm, txt);
  }
  cfgLine(LANG_CFG_CONTESTNR_D, cfgContestNumber());
  cfgLine(LANG_CFG_TELETEXT_D, teletextOn() ? 1 : 0);
  cfgLine(LANG_CFG_ITS_D, itsEnabled() ? 1 : 0);
  cfgLine(LANG_CFG_VBI_D, advVbiOpen() ? 1 : 0);
  cfgLine(LANG_CFG_LUMA_D, advLumaFilterIndex());
  cfgLine(LANG_CFG_CHROMA_D, advChromaFilterIndex());
  cfgLine(LANG_CFG_G924BARS_D, cfgG924ChromaBars() ? 1 : 0);
  cfgLine(LANG_CFG_TCGAP2_D, cfgTestcardgAp2() ? 1 : 0);
  cfgLine(LANG_CFG_SLIDEFADE_D, cfgSlideFadeOn() ? 1 : 0);
  cfgLine(LANG_CFG_DSTAUTO_D, cfgDstAutoOn() ? 1 : 0);
  cfgLine(LANG_CFG_STARTCARD_D, cfgStartPattern() + 1);
  cfgLine(LANG_CFG_SLIDESHOW_D, slideshowOn ? 1 : 0);
  cfgLine(LANG_CFG_CARD_D, patternIndex + 1);
  cfgLine(LANG_CFG_DIRTY_D, cfgDirty() ? 1 : 0);
  cfgLine(LANG_CFG_DATEFMT_D, CFG_DATE_FORMAT);
  cfgLine(LANG_CFG_DISPLAYBRIGHT_D, cfgDisplayBrightness());
  {
    // The board's own clock, not the PC's: no battery, restarts at the
    // time in config.h after a power cut. The PC counts on from here.
    ClockTime t;
    clockGet(&t);
    cfgLine(LANG_CFG_CLOCK_04D_02D_02D, t.year, t.month, t.day,
            t.hour, t.min, t.sec);
  }

  // The names of the choices, so the PC tool need not carry a copy of
  // them and cannot fall out of step with the firmware.
  for (int i = 0; i < ASPECT_COUNT; i++) {
    Serial.print(LANG_ASPECT);
    Serial.print(i);
    Serial.print(' ');
    Serial.println(aspectName(i));
  }
  for (int i = 0; i < advLumaFilterCount(); i++) {
    Serial.print(LANG_LUMA);
    Serial.print(i);
    Serial.print(' ');
    Serial.println(advLumaFilterName(i));
  }
  for (int i = 0; i < advChromaFilterCount(); i++) {
    Serial.print(LANG_CHROMA);
    Serial.print(i);
    Serial.print(' ');
    Serial.println(advChromaFilterName(i));
  }

  // The encoder's analogue controls, one line each with range and
  // neutral value, so the tool can build a slider without knowing what
  // any of them are. Name last, since it holds spaces.
  for (int i = 0; i < advParamCount(); i++) {
    cfgLine(LANG_CONTROL_D_MIN_D_MAX, i, advParamMin(i),
            advParamMax(i), advParamNeutralValue(i), advParamStepSize(i), advParamValue(i));
    Serial.println(advParamName(i));
  }

  // One line a card, named fields, name last since it can hold spaces.
  // A hidden card (no ADC, cardIsAvailable()) is not listed at all;
  // the tool numbers cards by the leading field, so the gap is safe.
  for (int i = 0; i < PATTERN_COUNT; i++) {
    if (!cardIsAvailable(i)) continue;
    const int slot = customSlotOf(PATTERNS[i]);
    const uint32_t crc = cardCrcForPc(i);
    // 1: PM5644 16:9 chroma bars, 2: AP1/AP2 on Card G, 3: contest,
    // 4: BBC Test Card F/W's own photo switch, 5: the Sony PCM card's
    // 16 bit and pre-emphasis switches (its "option" bits 0 and 1).
    // Not a CARD_TRAIT bit, that byte is full; checked by pattern
    // identity instead.
    const uint8_t tr = cfgCardTraits(i);
    const int own = (tr & CARD_TRAIT_G924)    ? 1
                  : (tr & CARD_TRAIT_CARDG)   ? 2
                  : (tr & CARD_TRAIT_CONTEST) ? 3
                  : (PATTERNS[i] == &PATTERN_TESTCARDF || PATTERNS[i] == &PATTERN_TESTCARDW) ? 4
                  : (PATTERNS[i] == &PATTERN_SONYPCM) ? 5
                  : (PATTERNS[i] == &PATTERN_PCMAUDIO) ? 6
                                              : 0;
    // An empty photograph slot always reads as switched off; see
    // cardIsEnabled().
    const bool enab = cardIsEnabled(i);
    cfgLine(LANG_CARD_D_SHOWID_D_SHOWSUB,
            i + 1, cfgCardShowId(i) ? 1 : 0, cfgCardShowSub(i) ? 1 : 0, cfgCardTickerOf(i),
            cfgCardTickerY(i), cfgCardTickerFont(i), cfgCardTickerSpeed(i),
            cfgCardTickerSize(i), cfgCardTextFont(i),
            (cfgCardTraits(i) & CARD_TRAIT_FIXEDFONT) ? 1 : 0,
            cfgCardAspect(i), cfgCardInsert(i),
            cfgCardMovingLine(i) ? 1 : 0, enab ? 1 : 0, slot >= 0 ? 1 : 0, own,
            (int)cfgCardIdSeen(i), (int)cfgCardBoxSeen(i), (int)cfgCardLineSeen(i),
            (int)cfgCardSubSeen(i), cfgCardCustomPhoto(i) ? 1 : 0, cfgCardOption(i),
            (unsigned long)cfgCardColor(i, CFG_COLOR_TICKER),
            (unsigned long)cfgCardColor(i, CFG_COLOR_ID),
            (unsigned long)cfgCardColor(i, CFG_COLOR_SUB),
            (unsigned long)cfgCardColor(i, CFG_COLOR_INSERT), (unsigned long)crc);
    // An uploadable slot goes by what is in it, its file name, not
    // "Custom 7"; an empty one reads as empty.
    if (slot >= 0) {
      const char *file = customFileName(PATTERNS[i]);
      Serial.println(file ? file : LANG_EMPTY_SLOT);
    } else {
      Serial.println(PATTERNS[i]->name);
    }
  }
  Serial.println(LANG_CFG_END);
}

// Reads one "name=value". Returns nothing; it says on the port what it
// did, in the same words the other commands use, so the PC tool
// recognises trouble by "failed".
static void setSettingFromCommand(const char *line) {
  const char *eq = strchr(line, '=');
  if (!eq || eq == line) {
    Serial.println(LANG_CFG_FAILED_EXPECTED_NAME_VALUE);
    return;
  }
  char name[48];
  size_t n = (size_t)(eq - line);
  if (n >= sizeof(name)) {
    Serial.println(LANG_CFG_FAILED_THE_NAME_IS);
    return;
  }
  memcpy(name, line, n);
  name[n] = 0;
  const char *val = eq + 1;
  const int num = atoi(val);

  // portrait.<target>=1 starts the upload of a BBC Test Card F/W photo
  // replacement (target 0 or 1); see portraitUploadFlow() for why this
  // does not go through the 'u' key like a Custom slot's own upload.
  if (!strncmp(name, "portrait.", 9)) {
    if (num == 0) {
      Serial.println(LANG_CFG_FAILED_EXPECTED_NAME_VALUE);
      return;
    }
    portraitUploadFlow(atoi(name + 9));
    return;
  }

  // Card scoped: card.<number>.<field>, counted from 1.
  if (!strncmp(name, "card.", 5)) {
    const char *p = name + 5;
    const int card = atoi(p) - 1;
    const char *dot = strchr(p, '.');
    if (!dot || card < 0 || card >= PATTERN_COUNT) {
      Serial.println(LANG_CFG_FAILED_NO_SUCH_CARD);
      return;
    }
    // What the card on screen has to redo, and only that: a full
    // cfgReload() per field would black the picture on every step of
    // dragging the band, unlike key y on the board, and the two sides
    // must behave the same.
    bool needsInit = false;   // the card lays itself out around this
    bool needsBand = false;   // only the band has to be built again
    bool needsText = false;   // only the text layout, the light reload
    bool needsInk = false;    // only the cached lettering inks

    const char *field = dot + 1;
    if (!strcmp(field, "showid")) {
      cfgSetCardShowId(card, num != 0);
      needsText = true;
    } else if (!strcmp(field, "showsub")) {
      cfgSetCardShowSub(card, num != 0);
      needsText = true;
    } else if (!strcmp(field, "ticker")) {
      // Transparent is not offered on an uploaded photograph; see the
      // note at the n key.
      if (num == 2 && customSlotOf(PATTERNS[card]) >= 0) {
        Serial.println(LANG_CFG_FAILED_TRANSPARENT_DOES_NOT);
        return;
      }
      cfgSetCardTickerOf(card, num);
      needsBand = true;
    } else if (!strcmp(field, "tickery")) {
      cfgSetCardTickerY(card, num);
      needsBand = true;
    } else if (!strcmp(field, "tickerspeed")) {
      // Nothing to lay out again: tickerFrame() reads the value every
      // picture, so the band changes pace or direction on the next one.
      cfgSetCardTickerSpeed(card, num);
    } else if (!strcmp(field, "tickerscale")) {
      // Wire name kept as "tickerscale" for protocol continuity, though
      // the value is now which of five fixed sizes (0..4), not a
      // percent. Changes the band height and every glyph's advance, so
      // the layout has to be built again, same as tickery/tickerfont.
      cfgSetCardTickerSize(card, num);
      needsBand = true;
    } else if (!strcmp(field, "textfont")) {
      if (num < 0 || num >= FONT_COUNT) {
        Serial.println(LANG_CFG_FAILED_NO_SUCH_FONT);
        return;
      }
      cfgSetCardTextFont(card, num);
      needsText = true;   // the take-up below applies the face first
    } else if (!strcmp(field, "tickerfont")) {
      cfgSetCardTickerFont(card, num);
      needsBand = true;
    } else if (!strcmp(field, "aspect")) {
      cfgSetCardAspect(card, num);
      needsInit = true;
    } else if (!strcmp(field, "insert")) {
      cfgSetCardInsert(card, num);
      needsInit = true;
    } else if (!strcmp(field, "moving")) {
      // The same refusal the m key gives: on a card without a box the
      // setting would go to flash and change nothing on screen. 1 means
      // "no box"; see cfgCardLineSeen().
      if (cfgCardLineSeen(card) == 1) {
        Serial.println(LANG_CFG_FAILED_THIS_CARD_HAS);
        return;
      }
      // Nothing has to be laid out again: the stripe reads the flag
      // once per picture in movingLineFrame().
      cfgSetCardMovingLine(card, num != 0);
    } else if (!strcmp(field, "option")) {
      // The card's own switches, meaning per card (see "own" in the
      // card line): the Sony PCM card reads bit 0 (16 bit) and bit 1
      // (pre-emphasis) in its init(), so a change goes through a fresh
      // init, the same take-up as aspect/insert.
      cfgSetCardOption(card, num);
      needsInit = true;
    } else if (!strcmp(field, "customphoto")) {
      // BBC Test Card F/W only; refused elsewhere, the same rule as
      // "moving" above, so a setting that could never show anything
      // does not silently go to flash.
      if (PATTERNS[card] != &PATTERN_TESTCARDF && PATTERNS[card] != &PATTERN_TESTCARDW) {
        Serial.println(LANG_CFG_FAILED_THIS_CARD_HAS);
        return;
      }
      // Nothing to lay out again: testcardfRenderRow()/testcardwRenderRow()
      // read the flag every row.
      cfgSetCardCustomPhoto(card, num != 0);
    } else if (!strcmp(field, "enabled")) {
      // An empty photograph slot always reads as off (see the dump), so
      // a set on it would be a switch that does nothing; refused, the
      // same answer the tool's greyed box gives.
      const int slotOf = customSlotOf(PATTERNS[card]);
      if (slotOf >= 0 && !photoValid(slotOf)) {
        Serial.println(LANG_CFG_FAILED_AN_EMPTY_SLOT);
        return;
      }
      // The card on screen stays up even when switched off: taking away
      // a picture somebody is looking at decides for them.
      cfgSetCardEnabled(card, num != 0);
    } else if (!strcmp(field, "tickercolor") || !strcmp(field, "idcolor") ||
               !strcmp(field, "subcolor") || !strcmp(field, "inscolor")) {
      // Six hex digits, RRGGBB, exactly what the card dump sends out.
      const int kind = !strcmp(field, "tickercolor") ? CFG_COLOR_TICKER
                     : !strcmp(field, "idcolor")     ? CFG_COLOR_ID
                     : !strcmp(field, "subcolor")    ? CFG_COLOR_SUB
                                                     : CFG_COLOR_INSERT;
      cfgSetCardColor(card, kind, (uint32_t)strtoul(val, nullptr, 16) & 0xFFFFFFu);
      // The ticker ink and its lut are worked out in the band layout;
      // the other three sit in the cache that textInkRefresh() fills.
      if (kind == CFG_COLOR_TICKER) needsBand = true;
      else needsInk = true;
    } else {
      Serial.println(LANG_CFG_FAILED_NO_SUCH_CARD_2);
      return;
    }
    // The card on screen has to take it up at once.
    if (card == patternIndex) {
      if (needsInk) textInkRefresh();
      if (needsBand) tickerBegin();
      if (needsText) {
        // The face first, so the reload lays the text out in it; for
        // the fields that left the face alone this sets what is set.
        textSetFont(cfgCardTextFont(-1));
        cfgReloadText();
      }
      if (needsInit) {
        aspectSet(cfgCardAspect(-1));
        insertSetMode(cfgCardInsert(-1));
        // The band sits in front of the aspect ratio, so it has to be
        // laid out again after those two.
        tickerBegin();
        cfgReload();
      }
    }
    // The new checksum goes back with the answer, so the PC knows
    // whether its kept picture of this card is still good without
    // reading the whole dump, which costs black rows on the port.
    cfgLine(LANG_CFG_SET_S_CRC_08LX, name, (unsigned long)cardCrcForPc(card));
    return;
  }

  // Not a setting but a question: cardcrc=7 answers with card 7's
  // checksum, riding the set command for a shared answer shape. Lets
  // the PC check whether a shared setting changed its picture without
  // the black rows a full dump costs.
  if (!strcmp(name, "cardcrc")) {
    const int i = num - 1;
    if (i < 0 || i >= PATTERN_COUNT) {
      Serial.println(LANG_CFG_FAILED_NO_SUCH_CARD);
      return;
    }
    cfgLine(LANG_CFG_SET_CARDCRC_CRC_08LX, (unsigned long)cardCrcForPc(i));
    return;
  }

  // Also a question, not a setting, the value is ignored: which card
  // is showing right now. Lets the PC tool follow a slideshow the
  // board steps through on its own, by polling instead of a full dump.
  if (!strcmp(name, "currentcard")) {
    cfgLine(LANG_CFG_SET_CURRENTCARD_CARD_D, patternIndex + 1);
    return;
  }

  // A control by number: control.3=12. The tool reads the range out of
  // the dump.
  if (!strncmp(name, "control.", 8)) {
    const int i = atoi(name + 8);
    if (i < 0 || i >= advParamCount()) {
      Serial.println(LANG_CFG_FAILED_NO_SUCH_CONTROL);
      return;
    }
    advParamSet(i, num);
    cfgLine(LANG_CFG_SET_S, name);
    return;
  }

  // A teletext row by number, 1..TT_ROWS: teletext.3=some text. An
  // empty value clears the row back to the built-in default page.
  if (!strncmp(name, "teletext.", 9)) {
    const int row = atoi(name + 9);
    if (row < 1 || row > TT_ROWS) {
      Serial.println(LANG_CFG_FAILED_NO_SUCH_TELETEXT);
      return;
    }
    cfgSetTeletextRow(row, val);
    teletextApplyRow(row);
    teletextRerasterRow(row);   // the waveform too, or the edit only airs after a reboot
    cfgLine(LANG_CFG_SET_S, name);
    return;
  }

  if (!strcmp(name, "slideshow")) {
    // Same switch as key s. Refuses when there is nothing to show, and
    // that refusal must reach the PC as a failure, or its button would
    // read "running" over a still card.
    slideshowSet(num != 0);
    if ((num != 0) != slideshowOn) {
      Serial.println(LANG_CFG_FAILED_NO_SEQUENCE_AND);
      return;
    }
  } else if (!strcmp(name, "textid")) {
    cfgSetTextId(val);
  } else if (!strcmp(name, "textsub")) {
    cfgSetTextSub(val);
  } else if (!strcmp(name, "tickertext")) {
    tickerSetText(val);
  } else if (!strcmp(name, "contest1")) {
    cfgSetContestText1(val);
  } else if (!strcmp(name, "contest2")) {
    cfgSetContestText2(val);
  } else if (!strcmp(name, "pcmtext")) {
    cfgSetPcmText(val);
  } else if (!strcmp(name, "insertleft")) {
    cfgSetInsertLeft(val);
  } else if (!strcmp(name, "insertright")) {
    cfgSetInsertRight(val);
  } else if (!strcmp(name, "contestnr")) {
    cfgSetContestNumber(num);
  } else if (!strcmp(name, "teletext")) {
    teletextSetOn(num != 0);
  } else if (!strcmp(name, "its")) {
    itsSetEnabled(num != 0);
  } else if (!strcmp(name, "vbi")) {
    advSetVbiOpen(num != 0);
  } else if (!strcmp(name, "luma")) {
    advSetLumaFilter(num);
  } else if (!strcmp(name, "chroma")) {
    advSetChromaFilter(num);
  } else if (!strcmp(name, "displaybright")) {
    cfgSetDisplayBrightness(num);
  } else if (!strcmp(name, "g924bars")) {
    cfgSetG924ChromaBars(num != 0);
  } else if (!strcmp(name, "tcgap2")) {
    cfgSetTestcardgAp2(num != 0);
  } else if (!strcmp(name, "slidefade")) {
    cfgSetSlideFadeOn(num != 0);
  } else if (!strcmp(name, "dstauto")) {
    cfgSetDstAutoOn(num != 0);
  } else if (!strcmp(name, "card")) {
    // Which card is SHOWING, so the tool can walk through them. A
    // hidden card cannot be summoned, not even by number.
    if (num >= 1 && num <= PATTERN_COUNT && cardIsAvailable(num - 1)) requestedPattern = num - 1;
  } else {
    Serial.println(LANG_CFG_FAILED_NO_SUCH_SETTING);
    return;
  }
  // No blanket cfgReload() here, or a shared setting would flicker the
  // picture like a card change every time; the setters that need it
  // set the flag themselves. No checksum either: a shared setting can
  // touch any card, so the PC catches up at Connect and after a save.
  cfgLine(LANG_CFG_SET_S, name);
}

// Sends a card as it really is: 576 rows of 720 samples in 4:2:2, the
// same bytes that go out over the pixel bus, so the PC never needs a
// second renderer that could disagree with the board. Costs a card
// change: loads the one asked for and puts back the one that was
// showing, up to a quarter second black each way.

// Sends one of the faces, so the PC tool draws with the board's own
// letters. Plain text, one number per character (the set holds the
// space and the slashed O at 0x01, neither fit for splitting on spaces).
static void fontDownload() {
  if (!Serial) return;
  Serial.println(LANG_FONT_READY_SEND_THE_FACE);
  const int idx = readNumber(LANG_FONT, FONT_COUNT);
  if (idx < 0) return;
  const Font *f = &FONTS[idx];
  cfgLine(LANG_FONT_D_ROWS_D_SX, idx, f->rows, f->sx,
          f->sy, f->digits, f->gap, f->count);
  Serial.println(f->name);
  for (int i = 0; i < f->count; i++) {
    const Glyph *g = &f->g[i];
    char line[16 + FONT_MAX_ROWS * 9];
    int n = snprintf(line, sizeof(line), LANG_GLYPH_D_D, (int)(uint8_t)g->c, (int)g->cols);
    for (int r = 0; r < f->rows && n > 0 && n < (int)sizeof(line); r++) {
      n += snprintf(line + n, sizeof(line) - (size_t)n, LANG_08LX, (unsigned long)g->r[r]);
    }
    if (n > 0 && n < (int)sizeof(line)) {
      Serial.write((const uint8_t *)line, (size_t)n);
      Serial.println();
    }
  }
  Serial.println(LANG_FONT_END);
  // Sending a face is about seven kilobytes with core0 building no rows
  // meanwhile, so it lags afterwards just as the card dump does.
  resyncIfBehind();
}

// --- the card dump as a job -------------------------------------------
//
// The dump runs as a job in the producer's spare row slots, like a
// card change's table copy, so the picture stays up while it sends;
// only the ticker, clock and stripe hold still (left out of what goes
// over). dumpActive itself is declared up by report(), which silences
// itself while the stream runs.
static int dumpRow = 0;
static int dumpReturnCard = -1;   // the card to go back to, or -1
static uint32_t dumpPacked = 0, dumpSent = 0;
static uint32_t dumpLastProgressMs = 0;
// AS WORDS AND NOT AS BYTES. pattern.h promises every renderer that
// base is aligned to four, and some of them write whole words.
static uint32_t dumpRowWords[SCREEN_W / 2];
static uint8_t dumpPackedBuf[(SCREEN_W / 2) * 5];

// 0 while the job runs; 1 done, 2 aborted. The job only ever SETS it:
// the finishing itself happens from the frame hook, between two
// pictures, because lifting the ticker pause halfway through a frame
// would give that one frame half a band.
static int dumpEndState = 0;
static const char *dumpEndWhy = nullptr;

static void cardDumpFinish() {
  const bool ok = dumpEndState == 1;
  dumpActive = false;
  dumpEndState = 0;
  tickerPause(false);
  movingLinePause(false);
  insertSetBlank(false);
  displaySetBusy(nullptr);
  // No tick here: the frame hook runs the card's tick every picture
  // anyway, so the date, the time and the stripe are back in the very
  // next one.
  //
  // The way back goes through the normal card change request, so the
  // frame hook does the switch with all its care; selectPattern() from
  // here would do its flash write in the middle of the frame gap.
  if (dumpReturnCard >= 0 && requestedPattern < 0) requestedPattern = dumpReturnCard;
  dumpReturnCard = -1;
  if (ok) {
    Serial.println();
    Serial.println(LANG_CARDDUMP_DONE);
  } else {
    Serial.printf(LANG_CARDDUMP_ABORTED_AT_ROW_D, dumpRow,
                  dumpEndWhy ? dumpEndWhy : "?");
  }
}

// One piece of the dump, from a spare row slot in loop(). Either moves
// packed bytes towards the port or renders and packs the next row,
// never both: each is bounded, and the slot has one row of margin.
static void cardDumpStep() {
  if (dumpEndState) return;   // waiting for the frame hook to finish up
  if (dumpSent < dumpPacked) {
    // Only as much as the buffer takes: a write that fits cannot
    // block, and blocking here would be the old black screen through
    // the back door.
    const int room = Serial.availableForWrite();
    if (room > 0) {
      uint32_t n = dumpPacked - dumpSent;
      if (n > (uint32_t)room) n = (uint32_t)room;
      Serial.write(dumpPackedBuf + dumpSent, n);
      dumpSent += n;
      dumpLastProgressMs = millis();
    } else if (millis() - dumpLastProgressMs > 2000) {
      dumpEndState = 2;
      dumpEndWhy = LANG_PORT_STOPPED_TAKING_DATA;
    }
    return;
  }
  if (dumpRow >= SCREEN_H) {
    dumpEndState = 1;
    return;
  }
  // A new row only once the tables are fully in RAM: rendering it from
  // flash costs more than the slot has.
  if (romCopyPending()) return;
  // The bare card, without aspect ratio or ticker: the PC applies the
  // letterbox and squeeze itself.
  activePattern->renderRow((uint8_t *)dumpRowWords, dumpRow);
  // Run length coded: a count of 1-255 followed by the 32 bit word it
  // repeats; see CardCache.cs on the PC side.
  const uint32_t *w = dumpRowWords;
  uint32_t n = 0;
  for (int x = 0; x < SCREEN_W / 2;) {
    const uint32_t v = w[x];
    int run = 1;
    while (x + run < SCREEN_W / 2 && w[x + run] == v && run < 255) run++;
    dumpPackedBuf[n++] = (uint8_t)run;
    memcpy(dumpPackedBuf + n, &v, 4);
    n += 4;
    x += run;
  }
  dumpPacked = n;
  dumpSent = 0;
  dumpRow++;
  dumpLastProgressMs = millis();
}

static void cardDownload() {
  if (!Serial) return;
  if (dumpActive) {
    Serial.println(LANG_CARDDUMP_FAILED_A_DUMP_IS);
    return;
  }
  Serial.println(LANG_CARDDUMP_READY_SEND_THE_CARD);
  const int card = readNumber(LANG_CARDDUMP, PATTERN_COUNT);
  if (card < 0) return;
  if (!cardIsAvailable(card)) return;   // never listed, so never asked for

  const int was = patternIndex;
  if (card != was) {
    selectPattern(card);
  } else if (cfgReloadNeeded()) {
    // A setting just changed on this card, and its normal init would
    // run later than the PC's near-instant fetch; without this it
    // would send the layout the card had before the change.
    cfgReloadDone();
    activePattern->init();
  } else if (cfgReloadTextNeeded()) {
    // Light variant of the same, for a text switch.
    cfgReloadTextDone();
    if (activePattern->reflow) activePattern->reflow();
    else activePattern->init();
  }
  // Tables need not be forced in: the job below only renders once
  // romCopyPending() says the copy is done.

  // Without the ticker or the moving line: both animate, so the PC
  // draws them itself and a baked-in snapshot would be wrong at once.
  tickerPause(true);
  movingLinePause(true);

  // And without the date and time, which change every second; the
  // black boxes stay, since they belong to the card, and go over the
  // port so the PC can put a running clock in them.
  insertSetBlank(true);
  if (activePattern->tick) activePattern->tick();

  Serial.printf(LANG_CARDDUMP_CARD_D_RLE_D, card + 1,
                (int)SCREEN_W, (int)SCREEN_H, (unsigned long)cfgCardCrc(card));
  if (insertInUse()) {
    int lx0, lx1, rx0, rx1, y0, y1;
    insertGetBoxes(&lx0, &lx1, &rx0, &rx1, &y0, &y1);
    Serial.printf(LANG_CARDDUMP_BOXES_D_D_D, lx0, lx1, rx0, rx1, y0, y1,
                  insertGetMode());
  } else {
    Serial.println(LANG_CARDDUMP_BOXES_NONE);
  }
  // The moving line's box goes with the picture too, so the PC knows
  // where to animate the stripe it left out.
  if (movingLineInUse()) {
    int mx0, mx1, my0, my1;
    movingLineGetBox(&mx0, &mx1, &my0, &my1);
    Serial.printf(LANG_CARDDUMP_LINEBOX_D_D_D, mx0, mx1, my0, my1);
  } else {
    Serial.println(LANG_CARDDUMP_LINEBOX_NONE);
  }

  // Fullscreen on the status panel for as long as the dump runs; picked
  // up by the normal per-picture displayFrameHook(), since the dump is
  // a job and the picture keeps playing. Same name-without-brackets
  // rule as updateDisplayLines(), so a photo slot shows its file name.
  {
    const char *fileName = customFileName(PATTERNS[card]);
    const char *name = fileName ? fileName : PATTERNS[card]->name;
    const char *bracket = strstr(name, " (");
    int nameLen = bracket ? (int)(bracket - name) : (int)strlen(name);
    char buf[48];
    snprintf(buf, sizeof(buf), LANG_DOWNLOADING_S, nameLen, name);
    displaySetBusy(buf);
  }

  // From here the dump is a job (see the block above cardDownload()):
  // rows follow in the producer's spare slots, the pauses above stay
  // on until cardDumpFinish() lifts them and returns to the showing card.
  dumpReturnCard = (card != was) ? was : -1;
  dumpRow = 0;
  dumpPacked = 0;
  dumpSent = 0;
  dumpLastProgressMs = millis();
  dumpActive = true;
}

static void handleSerialChar(int ch) {
  if (cmdLen >= 0) {
    cmdLastMs = millis();
    if (ch == 27) {
      cmdAbort(LANG_CANCELLED);
    } else if (ch == '\r' || ch == '\n') {
      cmdLine[cmdLen] = 0;
      cmdLen = -1;

      // A line that did not fit is refused rather than used short, or
      // the board would silently keep less than was typed. "failed" is
      // deliberate: the PC tool recognises trouble by it.
      if (cmdOverflow) {
        cmdOverflow = false;
        Serial.printf(LANG_INPUT_FAILED_THE_LINE_IS,
                      (int)sizeof(cmdLine) - 1);
        return;
      }

      if (cmdKind == 'N') {
        // On the contest page N and S apply to the large lines there;
        // on every other card, to the two insert boxes.
        if (activePattern == &PATTERN_CONTEST) {
          cfgSetContestText1(cmdLine);
          report(LANG_CONTEST_TOP_LINE_S, cfgContestText1());
        } else {
          cfgSetTextId(cmdLine);
          report(LANG_TEXT_TOP_BOX_S, cfgTextId());
        }
      } else if (cmdKind == 'S') {
        if (activePattern == &PATTERN_CONTEST) {
          cfgSetContestText2(cmdLine);
          report(LANG_CONTEST_BOTTOM_LINE_S, cfgContestText2());
        } else {
          cfgSetTextSub(cmdLine);
          report(LANG_TEXT_BOTTOM_BOX_S, cfgTextSub());
        }
      } else if (cmdKind == 'C') {
        cfgSetContestNumber(atoi(cmdLine));
        report(LANG_CONTEST_NUMBER_04D, cfgContestNumber());
      } else if (cmdKind == 'Q') {
        setSequenceFromCommand(cmdLine);
      } else if (cmdKind == 'K') {
        setSettingFromCommand(cmdLine);
      } else if (cmdKind == 'W') {
        tickerSetText(cmdLine);
        Serial.print(LANG_TICKER_TEXT);
        Serial.println(tickerText()[0] ? tickerText() : LANG_EMPTY);
      } else {
        setClockFromCommand(cmdLine);
      }
    } else if (cmdLen < (int)sizeof(cmdLine) - 1) {
      cmdLine[cmdLen++] = (char)ch;
    } else {
      cmdOverflow = true;
    }
    return;
  }

  if (ch >= '1' && ch <= '0' + advLumaFilterCount()) {
    advSetLumaFilter(ch - '1');
    printLumaFilter();
  } else if (ch == 'x') {
    advSetChromaFilter((advChromaFilterIndex() + 1) % advChromaFilterCount());
    printChromaFilter();
  } else if (ch == 'b') {
    cfgSetG924ChromaBars(!cfgG924ChromaBars());
    report(LANG_G924_CHROMA_TEST_BARS_S, cfgG924ChromaBars() ? LANG_ON : LANG_OFF);
    if (activePattern != &PATTERN_PM5644G924) {
      report(LANG_ONLY_ON_THE_PM5644);
    } else {
      report(LANG_NARROW_BARS_AT_X);
    }
  } else if (ch == 'f') {
    // Kept per card, like the ticker's face; cycles the card on screen.
    const int next = (cfgCardTextFont(-1) + 1) % FONT_COUNT;
    cfgSetCardTextFont(-1, next);
    textSetFont(next);
    cfgReloadText();
    report(LANG_FONT_S_D_D_ON, textFontName(next), next + 1,
         FONT_COUNT, activePattern->name);
    report(LANG_PM5544_WAS_TRACED_FROM);
  } else if (ch == 'a') {
    // One key to choose and two to turn, instead of a key per control.
    advParamSelect((advParamIndex() + 1) % advParamCount());
    printParam();
  } else if (ch == '+' || ch == '=') {
    advParamStep(+1);
    printParam();
  } else if (ch == '-' || ch == '_') {
    advParamStep(-1);
    printParam();
  } else if (ch == '0') {
    advParamNeutral();
    printParam();
  } else if (ch == 'v') {
    advSetVbiOpen(!advVbiOpen());
    printVbi();
  } else if (ch == 'e') {
    teletextSetOn(!teletextOn());
    printTeletext();
    // The data sits in the blanking, so without the VBI pass-through
    // nothing comes out and the picture does not show that it is on.
    if (teletextOn() && !advVbiOpen()) {
      report(LANG_NOTE_THE_VBI_IS);
    }
  } else if (ch == 'I') {
    itsSetEnabled(!itsEnabled());
    printIts();
    // Like teletext this sits in the blanking: without the VBI
    // pass-through nothing comes out and the picture does not show that
    // it is on.
    if (itsEnabled() && !advVbiOpen()) {
      report(LANG_NOTE_THE_VBI_IS_2);
    }
    // The packet layout of teletext shifts with it, so report that
    // again to show what is left of the page.
    if (teletextOn()) printTeletext();
  } else if (ch == 'u') {
    photoUpload();
  } else if (ch == 'h') {
    // First line only; the second has its own key, q. Not every card
    // has a place for it, so say so rather than write a setting that
    // changes nothing on screen.
    if (!cfgCardHasId()) {
      report(LANG_ID_S_HAS_NO_PLACE,
             activePattern->name);
    } else {
      // Light reload: only the text layout, not the full init.
      cfgSetShowId(!cfgShowId());
      cfgReloadText();
      report(LANG_ID_FIRST_LINE_S_ON, cfgShowId() ? LANG_ON_CAPS : LANG_OFF);
    }
  } else if (ch == 'q') {
    // The second line, the same way.
    if (!cfgCardHasSub()) {
      report(LANG_ID_S_HAS_NO_PLACE_2,
             activePattern->name);
    } else {
      cfgSetShowSub(!cfgShowSub());
      cfgReloadText();
      report(LANG_ID_SECOND_LINE_S_ON, cfgShowSub() ? LANG_ON_CAPS : LANG_OFF);
    }
  } else if (ch == 'n') {
    // Off, a bar of its own, straight over the card, round again; kept
    // per card. Transparent is skipped on an uploaded photograph: the
    // band on top of a flash-read row costs more than a row has.
    const bool photo = customSlotOf(activePattern) >= 0;
    int next = (tickerMode() + 1) % TICKER_MODE_COUNT;
    if (photo && next == TICKER_TRANSPARENT) next = TICKER_OFF;
    tickerSetMode(next);
    report(LANG_TICKER_S_ON_S_S, tickerModeName(), activePattern->name,
           photo ? LANG_TRANSPARENT_NOT_ON_PHOTO : "");
  } else if (ch == 'm') {
    // The moving line: a white stripe sweeping through a black box,
    // one second per sweep. Only a card with a box for it has this.
    if (!movingLineInUse()) {
      report(LANG_LINE_S_HAS_NO_PLACE,
             activePattern->name);
    } else {
      cfgSetCardMovingLine(-1, !cfgCardMovingLine(-1));
      report(LANG_LINE_MOVING_LINE_S_ON, cfgCardMovingLine(-1) ? LANG_ON_CAPS : LANG_OFF,
             activePattern->name);
    }
  } else if (ch == 'y' || ch == 'Y') {
    // Two rows at a time, because tickerSetY() rounds down to an even
    // row anyway: an odd one would put the two edges of the band in
    // different fields and a horizontal edge that changes field
    // flickers at 25 Hz.
    tickerSetY(tickerY() + (ch == 'y' ? -2 : 2));
    report(LANG_TICKER_BAND_AT_ROW_D, tickerY(), tickerY() + tickerHeight() - 1);
  } else if (ch == 'Z') {
    tickerSetFont((tickerFont() + 1) % FONT_COUNT);
    report(LANG_TICKER_FONT_S_BAND_D, tickerFontName(), tickerHeight(),
           tickerY());
  } else if (ch == 's') {
    slideshowSet(!slideshowOn);
  } else if (ch == 'l') {
    photoListSlots();
  } else if (ch == 'X') {
    cardDownload();
  } else if (ch == 'c') {
    fontDownload();
  } else if (ch == 'D') {
    photoDownload();
  } else if (ch == 'E') {
    photoEraseSlot();
  } else if (ch == 'U') {
    printPhotoSlots();
  } else if (ch == 'F') {
    cfgFactoryReset();
    report(LANG_STORAGE_BACK_TO_THE_FACTORY);
  } else if (ch == 'g') {
    // Same ROM USB bootloader BOOTSEL drops into, so the PC tool can
    // write a new UF2 without anyone touching the board.
    Serial.println(LANG_RESET_REBOOTING_INTO_THE_USB);
    Serial.flush();
    rp2040.rebootToBootloader();
  } else if (ch == 'z') {
    sflTimingReset();
  } else if (ch == 'A') {
    printSettings();
  } else if (ch == '*') {
    // Writing is its own command, not something every key does: an
    // erase blacks the picture for about 56 ms, so walking through
    // settings with a key would blink the screen each step. Which card
    // is on is the exception, written at once, so a board switched off
    // right after a change comes back up on it.
    if (!cfgDirty()) {
      report(LANG_STORAGE_NOTHING_HAS_CHANGED);
    } else if (saveNow()) {
      report(LANG_STORAGE_SAVED);
    }
  } else if (ch == 'i') {
    // The cycle walks the first three modes; anyone sitting in the
    // fixed text rejoins it through 0.
    insertSetMode(insertGetMode() >= INSERT_CYCLE ? 0 : (insertGetMode() + 1) % INSERT_CYCLE);
    cfgSetCardInsert(-1, insertGetMode());
    printInserts();
    if (activePattern != &PATTERN_PM5644G00) {
      report(LANG_ONLY_VISIBLE_ON_THE);
    }
  } else if (ch == 'd') {
    timingOn = !timingOn;
    // With the build stamp, so a measurement can never be read against
    // the wrong firmware: numbers that refuse to move have already sent
    // an afternoon chasing code that was not on the board.
    Serial.printf(LANG_TIMING_REPORT_S_BUILD_S, timingOn ? LANG_ON : LANG_OFF,
                  __DATE__, __TIME__);
  } else if (ch == 'w') {
    // Kept per card: a 16:9 card wants the 16:9 word, a 4:3 one the
    // 4:3 word.
    aspectSet((aspectGet() + 1) % ASPECT_COUNT);
    cfgSetCardAspect(-1, aspectGet());
    report(LANG_ASPECT_D_D_S, aspectGet() + 1, ASPECT_COUNT, aspectName(aspectGet()));
  } else if (ch == 'p') {
    if (requestedPattern < 0) requestedPattern = nextPattern(patternIndex, 1);
  } else if (ch == 'P') {
    if (requestedPattern < 0) requestedPattern = nextPattern(patternIndex, -1);
  } else if (ch == 'T' || ch == 't' || ch == 'N' || ch == 'S' || ch == 'C' || ch == 'Q' ||
             ch == 'W' || ch == 'K') {
    // The rest of the line is text. Worth announcing, because until the
    // Enter the other keys do nothing.
    cmdKind = (ch == 't') ? 'T' : (char)ch;
    cmdLen = 0;
    cmdOverflow = false;
    cmdLastMs = millis();
    Serial.printf(LANG_S_TYPE_AND_FINISH,
                  cmdKind == 'T'   ? LANG_KIND_CLOCK
                  : cmdKind == 'N' ? LANG_KIND_TOP_BOX
                  : cmdKind == 'Q' ? LANG_KIND_SEQUENCE
                  : cmdKind == 'W' ? LANG_KIND_TICKER
                  : cmdKind == 'K' ? LANG_KIND_CFG
                                   : LANG_KIND_BOTTOM_BOX);
    if (cmdKind == 'Q') {
      char now[SEQ_TEXT_MAX];
      cfgSeqText(now, sizeof(now));
      Serial.print(LANG_SEQ_NOW);
      Serial.println(now[0] ? now : LANG_NONE);
      Serial.println(LANG_SEQ_FORM_2S_1_3S);
    }
  } else if (ch == 'k') {
    ClockTime t;
    clockGet(&t);
    Serial.printf(LANG_CLOCK_04D_02D_02D_02D, t.year, t.month, t.day, t.hour, t.min,
                  t.sec);
  } else if (ch == 'r') {
    // Only writes the registers again (about 2 ms). If the picture
    // comes back from this, the encoder had lost its settings.
    advReapplyConfig();
    Serial.println(LANG_ADV_CONFIGURATION_WRITTEN_AGAIN);
  } else if (ch == 'R') {
    // A full hardware reset (about 100 ms; it interrupts the row
    // production briefly, so starves goes up from it, which is
    // expected).
    advHardRestart();
    Serial.println(LANG_ADV_HARDWARE_RESET_AND_RECONFIGURATION);
  } else if (ch == 'L') {
    // The alignment picture for the display. See displayTestCard(): all
    // four corners in view and the frame touching all four edges means
    // the offsets are right.
    displayTestCard();
    char buf[160];  // displayReport is a long line
    displayReport(buf, sizeof(buf));
    Serial.printf(LANG_DISPLAY_S, buf);
    Serial.println(LANG_O_O_COLUMN_1);
    Serial.println(LANG_G_SWAP_BGR_AND);
  } else if (ch == 'o' || ch == 'O' || ch == 'j' || ch == 'J') {
    displayNudge(ch == 'o' ? -1 : (ch == 'O' ? 1 : 0), ch == 'j' ? -1 : (ch == 'J' ? 1 : 0));
    displayTestCard();
    char buf[160];  // displayReport is a long line
    displayReport(buf, sizeof(buf));
    Serial.printf(LANG_DISPLAY_S, buf);
  } else if (ch == 'H' || ch == 'G' || ch == 'V' || ch == 'B') {
    if (ch == 'H') displayFlipOrientation();
    if (ch == 'G') displayToggleBgr();
    if (ch == 'V') displayToggleInvert();
    // B is the one to reach for when the panel lights up during the
    // bring-up and goes dark the moment it finishes: then the BL pin
    // goes the other way round from what config.h says.
    if (ch == 'B') displayFlipBacklight();
    displayTestCard();
    char buf[160];  // displayReport is a long line
    displayReport(buf, sizeof(buf));
    Serial.printf(LANG_DISPLAY_S, buf);
  } else if (ch == '?') {
    printHelp();
    Serial.println(LANG_HELP_STATE_OF_THINGS);
    printPatterns();
    printAspect();
    printInserts();
    printLumaFilter();
    printChromaFilter();
    // Only when there is one. A line saying a knob is not fitted is
    // noise on a board that never had one.
    if (encoderPresent()) {
      if (encoderCandidate >= 0) {
        Serial.printf(LANG_KNOB_OFFERING_D_D_S,
                      encoderCandidate + 1, PATTERN_COUNT, PATTERNS[encoderCandidate]->name);
      } else {
        Serial.println(LANG_KNOB_TURN_TO_CHOOSE_A);
      }
    }
    printTeletext();
    Serial.printf(LANG_INTERNAL_COLOUR_BARS_S, advColourBars() ? LANG_ON_CAPS : LANG_OFF);
    Serial.printf(LANG_TEXT_TOP_S_BOTTOM, cfgTextId(), cfgTextSub(),
                  !cfgCardHasId() ? LANG_ID_NO_PLACE_ON_CARD
                  : cfgShowId()   ? ""
                                  : LANG_ID_OFF_ON_THIS_CARD);
    Serial.printf(LANG_TIMING_REPORT_S, timingOn ? LANG_ON : LANG_OFF);
    if (cfgDirty()) Serial.println(LANG_SETTINGS_CHANGED_BUT_NOT);
    {
      Serial.printf(LANG_TICKER_S_ON_THIS,
                    tickerModeName(), tickerFontName(), tickerHeight(), tickerY());
      Serial.println(tickerText()[0] ? tickerText() : LANG_EMPTY);
    }
    {
      const int want = cfgStartPattern();
      const bool sane = want >= 0 && want < PATTERN_COUNT;
      Serial.printf(LANG_STARTS_ON_CARD_D, sane ? want + 1 : 1,
                    sane ? PATTERNS[want]->name : PATTERNS[0]->name);
    }
    {
      char now[SEQ_TEXT_MAX];
      cfgSeqText(now, sizeof(now));
      Serial.printf(LANG_SLIDESHOW_S_SEQUENCE, slideshowOn ? LANG_RUNNING : LANG_OFF);
      Serial.println(now[0] ? now : LANG_NONE_EVERY_FILLED_SLOT);
    }
    // Flash speed too, since the Custom cards need 22.5 MB/s reading
    // 1440 bytes a row. From the startup measurement, not remeasured
    // here, since that would drain the ring buffer.
    {
      unsigned div = flashSpeedDiv();
      unsigned kbs = flashSpeedLastKBs();
      Serial.printf(LANG_FLASH_DIVISOR_U_QSPI,
                    div, (unsigned long)(CFG_SYSCLK_HZ / div / 1000000),
                    (unsigned long)(CFG_SYSCLK_HZ / div / 100000) % 10, kbs / 1000, (kbs / 100) % 10);
    }
  }
}

// Watches the encoder configuration: a register reading back differently
// than what we wrote means a reset or a corrupted I2C transfer. One
// register a picture, repaired by rewriting the configuration.
static uint32_t cfgDeviations = 0;

// advReapplyConfig() costs ~8.7 ms against a ~3.9 ms ring buffer, so
// repair is bounded to once a second rather than run unbounded.
static const uint32_t ADV_REPAIR_MIN_MS = 1000;
static uint32_t advRepairAtMs = 0;

static void encoderConfigWatch() {
  uint8_t reg = 0, readBack = 0, expected = 0;
  if (advConfigCheckStep(&reg, &readBack, &expected)) return;

  cfgDeviations++;
  if ((int32_t)(videoProducedCount - videoConsumedCount) < VIDEO_ROM_COPY_KEEP_ROWS) return;
  if (advRepairAtMs != 0 && millis() - advRepairAtMs < ADV_REPAIR_MIN_MS) return;
  advRepairAtMs = millis();

  report(LANG_ADV_REGISTER_0X_02X_IS,
       reg, readBack, expected, (unsigned long)cfgDeviations);
  advReapplyConfig();
}


// --- THE ON-DEVICE MENU -------------------------------------------------
//
// Same options as PC Software/src/SystemTab.cs, minus the two PC-only
// buttons ("Fetch all again", "Log") which have no board-side meaning.
//
// The slashed O is left out of the letter picker: which single byte the
// on-device font's glyph table expects for it is not established here.
static const char MENU_CHARS[] =
    " !\"&'()+,-./0123456789:;=?ABCDEFGHIJKLMNOPQRSTUVWXYZ_";
static const int MENU_CHARS_N = sizeof(MENU_CHARS) - 1;

static int menuCharIndex(char c) {
  for (int i = 0; i < MENU_CHARS_N; i++) {
    if (MENU_CHARS[i] == c) return i;
  }
  return 0;   // not found: falls back to a space
}

typedef const char *(*MenuTextGet)();
typedef void (*MenuTextSet)(const char *);
// Labels match PC Software/src/UiText.cs, sentence case not shouted.
struct MenuTextField {
  const char *label;
  MenuTextGet get;
  MenuTextSet set;
  int maxLen;   // the box's own MaxLength in SystemTab.cs / BoardText.Allowed
};
static const MenuTextField MENU_TEXT_FIELDS[] = {
    {LANG_MENU_FIRST_LINE, cfgTextId, cfgSetTextId, 31},
    {LANG_MENU_SECOND_LINE, cfgTextSub, cfgSetTextSub, 31},
    {LANG_MENU_TICKER_TEXT, cfgTickerText, cfgSetTickerText, TICKER_MAX_CHARS},
    {LANG_MENU_TEXT_DATE, cfgInsertLeft, cfgSetInsertLeft, 31},
    {LANG_MENU_TEXT_TIME, cfgInsertRight, cfgSetInsertRight, 31},
};
static const int MENU_TEXT_COUNT = sizeof(MENU_TEXT_FIELDS) / sizeof(MENU_TEXT_FIELDS[0]);

typedef bool (*MenuBoolGet)();
typedef void (*MenuBoolSet)(bool);
struct MenuToggle {
  const char *label;
  MenuBoolGet get;
  MenuBoolSet set;
};
// "Teletext" is UiText.TeletextSwitch's first word, shortened for the
// panel; ITS and VBI stay upper case as abbreviations, not shouting.
static const MenuToggle MENU_TOGGLES[] = {
    {LANG_MENU_TELETEXT, teletextOn, teletextSetOn},
    {LANG_ITS, itsEnabled, itsSetEnabled},
    {LANG_MENU_VBI, advVbiOpen, advSetVbiOpen},
    // Matches UiText.DstAutoSwitch on the PC side.
    {LANG_MENU_AUTO_SUMMER_TIME, cfgDstAutoOn, cfgSetDstAutoOn},
};
static const int MENU_TOGGLE_COUNT = sizeof(MENU_TOGGLES) / sizeof(MENU_TOGGLES[0]);

typedef int (*MenuCycleIndex)();
typedef void (*MenuCycleSet)(int);
typedef int (*MenuCycleCount)();
typedef const char *(*MenuCycleName)(int);
struct MenuCycle {
  const char *label;
  MenuCycleIndex index;
  MenuCycleSet set;
  MenuCycleCount count;
  MenuCycleName name;
};
// Short names here, not advLumaFilterName()/advChromaFilterName(): far
// more than DISPFONT28 fits, and those carry the full datasheet reading.
static const MenuCycle MENU_CYCLES[] = {
    {LANG_MENU_LUMA_FILTER, advLumaFilterIndex, advSetLumaFilter, advLumaFilterCount,
     advLumaFilterShortName},
    {LANG_MENU_CHROMA_FILTER, advChromaFilterIndex, advSetChromaFilter, advChromaFilterCount,
     advChromaFilterShortName},
};
static const int MENU_CYCLE_COUNT = sizeof(MENU_CYCLES) / sizeof(MENU_CYCLES[0]);

// One slot each for brightness, the clock, enable/disable a card, then
// the analogue controls and Close. Nothing here is stored; menuKindAt()
// works it out from these counts and advParamCount() every time.
static const int MENU_BRIGHTNESS_INDEX = MENU_TEXT_COUNT + MENU_TOGGLE_COUNT + MENU_CYCLE_COUNT;
static const int MENU_DATETIME_INDEX = MENU_BRIGHTNESS_INDEX + 1;
static const int MENU_ENABLECARD_INDEX = MENU_DATETIME_INDEX + 1;
static const int MENU_STATIC_COUNT = MENU_ENABLECARD_INDEX + 1;
static int menuParamBase() { return MENU_STATIC_COUNT; }
static int menuCloseIndex() { return MENU_STATIC_COUNT + advParamCount(); }
static int menuTotalCount() { return menuCloseIndex() + 1; }

static int menuToggleIndex(int i) { return i - MENU_TEXT_COUNT; }
static int menuCycleIndexOf(int i) { return i - MENU_TEXT_COUNT - MENU_TOGGLE_COUNT; }
static int menuParamIndexOf(int i) { return i - menuParamBase(); }

// PLAIN int AND NOT enum: the .ino build tool writes its own function
// prototypes ahead of everything in this file, including any of ours,
// and a prototype for a function returning a type this file defines
// itself does not compile ("does not name a type") because that
// prototype lands before the type does. Measured here rather than
// looked up; see the same choice at MenuMode below.
typedef int MenuKind;
static const MenuKind MENU_KIND_TEXT = 0;
static const MenuKind MENU_KIND_TOGGLE = 1;
static const MenuKind MENU_KIND_CYCLE = 2;
static const MenuKind MENU_KIND_SLIDER = 3;
static const MenuKind MENU_KIND_PARAM = 4;
static const MenuKind MENU_KIND_CLOSE = 5;
static const MenuKind MENU_KIND_DATETIME = 6;
static const MenuKind MENU_KIND_CARDENABLE = 7;

// int and not MenuKind: the .ino build tool hoists a prototype for
// every function definition to a point before MenuKind's own typedef.
static int menuKindAt(int i) {
  if (i < MENU_TEXT_COUNT) return MENU_KIND_TEXT;
  if (i < MENU_TEXT_COUNT + MENU_TOGGLE_COUNT) return MENU_KIND_TOGGLE;
  if (i < MENU_TEXT_COUNT + MENU_TOGGLE_COUNT + MENU_CYCLE_COUNT) return MENU_KIND_CYCLE;
  if (i == MENU_BRIGHTNESS_INDEX) return MENU_KIND_SLIDER;
  if (i == MENU_DATETIME_INDEX) return MENU_KIND_DATETIME;
  if (i == MENU_ENABLECARD_INDEX) return MENU_KIND_CARDENABLE;
  if (i < menuCloseIndex()) return MENU_KIND_PARAM;
  return MENU_KIND_CLOSE;
}

static bool menuOpen = false;
// A short press opens the per-card menu, a long press the System menu.
static bool menuIsCard = false;
static int menuIndex = 0;
static uint32_t menuLastMs = 0;
static const uint32_t MENU_TIMEOUT_MS = 60000;

// Guards a rotary encoder push switch's own mechanical nudge to the
// shaft from being read as a deliberate turn, on both press and release.
static const uint32_t MENU_TURN_GUARD_MS = 150;
// The moment the guard was armed, not a deadline: a deadline tested
// with a signed subtraction is a 2^31 ms window that reads "armed"
// again 24.9 days after the last menu press (the encoder.cpp boot
// guard failed the same way). An age test on the arming moment is
// wrap-safe and expires by itself.
static uint32_t menuTurnGuardArmedMs = 0;

// Whether this hold of the knob already fired a long press, so a
// release after it does not also commit the item's short-press action.
static bool menuHeldForLongPress = false;

// The card menu's short press decides its action at press-down, but
// only opens once encoderTakeRelease() confirms the hold stayed short.
static bool cardMenuPendingOpen = false;

// How fast a long text field's preview scrolls in NAV mode.
static const uint32_t MENU_TEXT_SCROLL_STEP_MS = 200;
static const uint32_t MENU_TEXT_SCROLL_END_PAUSE = 2;

// See the note at MenuKind above for why int and not enum.
typedef int MenuMode;
static const MenuMode MENU_MODE_NAV = 0;
static const MenuMode MENU_MODE_ADJUST = 1;
static const MenuMode MENU_MODE_EDIT = 2;
static MenuMode menuMode = MENU_MODE_NAV;

static char menuEditBuf[TICKER_MAX_CHARS + 1];
static int menuEditCursor = 0;
static int menuEditMaxLen = 0;

// --- THE DATE AND TIME ITEM ----------------------------------------------
//
// Staged like a text field: menuOpenItem() copies the running clock in
// once, turning the knob only touches this copy, and clockSet() is not
// called until confirmed or the knob is held, so six dependent fields
// half typed in never lands in the real clock.
static int menuDtYear, menuDtMonth, menuDtDay, menuDtHour, menuDtMin, menuDtSec;
static int menuDtField = 0;   // 0 year, 1 month, 2 day, 3 hour, 4 min, 5 sec

static int menuDtDaysInMonth(int y, int m) {
  static const int8_t d[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
  return d[m];
}

// One field at a time, wrapping within its own range, except the year,
// which clamps. Changing month or year re-clamps the day to whatever
// the new month has, rather than wrapping or jumping to the 1st.
static void menuDtTurn(int turned) {
  switch (menuDtField) {
    case 0:
      menuDtYear += turned;
      if (menuDtYear < 2000) menuDtYear = 2000;
      if (menuDtYear > 2099) menuDtYear = 2099;
      break;
    case 1:
      menuDtMonth = ((menuDtMonth - 1 + turned) % 12 + 12) % 12 + 1;
      break;
    case 2: {
      const int n = menuDtDaysInMonth(menuDtYear, menuDtMonth);
      menuDtDay = ((menuDtDay - 1 + turned) % n + n) % n + 1;
      return;   // day itself, nothing left to re-clamp
    }
    case 3:
      menuDtHour = ((menuDtHour + turned) % 24 + 24) % 24;
      break;
    case 4:
      menuDtMin = ((menuDtMin + turned) % 60 + 60) % 60;
      break;
    default:
      menuDtSec = ((menuDtSec + turned) % 60 + 60) % 60;
      return;   // seconds, nothing to re-clamp either
  }
  const int n = menuDtDaysInMonth(menuDtYear, menuDtMonth);
  if (menuDtDay > n) menuDtDay = n;
}

// The value string, the selected field parenthesised: DISPFONT28's
// character set has "(" and ")" but not "[" or "]".
static void menuDtFormat(char *out, size_t outLen, int field,
                          int y, int mo, int d, int h, int mi, int s) {
  // dd-mm-yy, not ISO; the year still stages/stores as four digits.
  char yy[8], mm[6], dd[6], hh[6], ii[6], ss[6];
  snprintf(dd, sizeof(dd), field == 2 ? "(%02d)" : "%02d", d);
  snprintf(mm, sizeof(mm), field == 1 ? "(%02d)" : "%02d", mo);
  snprintf(yy, sizeof(yy), field == 0 ? "(%02d)" : "%02d", y % 100);
  snprintf(hh, sizeof(hh), field == 3 ? "(%02d)" : "%02d", h);
  snprintf(ii, sizeof(ii), field == 4 ? "(%02d)" : "%02d", mi);
  snprintf(ss, sizeof(ss), field == 5 ? "(%02d)" : "%02d", s);
  snprintf(out, outLen, "%s-%s-%s %s:%s:%s", dd, mm, yy, hh, ii, ss);
}

// --- ENABLING A CARD FROM THE KNOB ----------------------------------------
//
// The knob has no way to type a number and reach a disabled card
// directly, so this item cycles through PATTERN_COUNT independently of
// enabled state, only ever changing cfgCardEnabled(), never what is on air.
static int menuEnableCardIdx = 0;

// An empty photograph slot is skipped too: cardIsEnabled() already
// treats it as off regardless of the stored flag.
static bool menuEnableCardVisible(int i) {
  if (!cardIsAvailable(i)) return false;
  const int slot = customSlotOf(PATTERNS[i]);
  return slot < 0 || photoValid(slot);
}

// Same walk-and-wrap shape as nextPattern(), and the same "at most once
// round" bound, so an all-empty photograph bank cannot spin this
// forever -- it just settles back on a drawn card instead.
static int menuEnableCardStep(int from, int dir) {
  int i = from;
  for (int n = 0; n < PATTERN_COUNT; n++) {
    i = (i + dir + PATTERN_COUNT) % PATTERN_COUNT;
    if (menuEnableCardVisible(i)) return i;
  }
  return from;
}

// Same shape as enabledPatternCount()/enabledPatternRank() above, for
// this item's own "%d/%d" of cards rather than of system menu items.
static int menuEnableCardVisibleCount() {
  int n = 0;
  for (int i = 0; i < PATTERN_COUNT; i++) {
    if (menuEnableCardVisible(i)) n++;
  }
  return n;
}

static int menuEnableCardVisibleRank(int idx) {
  int n = 0;
  for (int i = 0; i <= idx; i++) {
    if (menuEnableCardVisible(i)) n++;
  }
  return n;
}

static void menuEnableCardFormat(char *out, size_t outLen) {
  // Self-correcting: a photo erased from the PC tool while looking at
  // this item could otherwise point at nothing.
  if (!menuEnableCardVisible(menuEnableCardIdx))
    menuEnableCardIdx = menuEnableCardStep(menuEnableCardIdx, 1);
  const int i = menuEnableCardIdx;
  const int slot = customSlotOf(PATTERNS[i]);
  // The file name alone for a photograph, not the generic "Custom N"
  // suffix that says nothing about which picture is which.
  const char *name = (slot >= 0) ? customFileName(PATTERNS[i]) : nullptr;
  if (!name) name = PATTERNS[i]->name;
  snprintf(out, outLen, "%s: %s", name, cfgCardEnabled(i) ? LANG_ENABLED : LANG_DISABLED);
}

// The same scroll-clock MENU_KIND_TEXT's own NAV-mode preview uses;
// shared here because this item needs it twice (ADJUST and NAV).
static int menuScrollCursor(const char *text) {
  const int len = (int)strlen(text);
  if (len <= 0) return -1;
  const uint32_t cycle = (uint32_t)len + MENU_TEXT_SCROLL_END_PAUSE;
  uint32_t t = (millis() / MENU_TEXT_SCROLL_STEP_MS) % cycle;
  if (t >= (uint32_t)len) t = (uint32_t)len - 1;
  return (int)t;
}

// --- THE PER-CARD MENU ---------------------------------------------------
//
// Same options as PC Software/src/CardsTab.cs, for whichever card is on
// screen when the knob is pressed. Which items apply depends on the
// active card, so this list is built fresh every time the menu opens;
// see cardMenuBuild() below.
//
// A few basic colours to pick with a knob rather than the PC tool's
// full RGB picker: the project's own EBU colour bar set.
struct CardColorChoice {
  const char *name;
  uint32_t rgb;
};
static const CardColorChoice CARD_COLORS[] = {
    {LANG_COLOR_WHITE, 0xFFFFFFu},   {LANG_COLOR_YELLOW, 0xFFFF00u}, {LANG_COLOR_CYAN, 0x00FFFFu},
    {LANG_COLOR_GREEN, 0x00FF00u},   {LANG_COLOR_MAGENTA, 0xFF00FFu}, {LANG_COLOR_RED, 0xFF0000u},
    {LANG_COLOR_BLUE, 0x0000FFu},
};
static const int CARD_COLOR_COUNT = sizeof(CARD_COLORS) / sizeof(CARD_COLORS[0]);

static const char *cardColorName(int i) {
  return (i >= 0 && i < CARD_COLOR_COUNT) ? CARD_COLORS[i].name : "?";
}

// Which PATTERNS[] index the menu is open for; read by every wrapper
// below instead of each taking a card argument, matching the System
// menu's own int()/void(int) table shape.
static int cardMenuCard = -1;

// The card on screen has to take a change up at once, the same helpers
// setSettingFromCommand()'s own card scoped set already calls (see its
// needsInk/needsBand/needsText/needsInit). cardMenuCard is patternIndex
// for the entire time this menu is open, so unlike that serial path,
// no "is this the card on screen" check is needed here, it always is.
static void cardMenuRefreshInk() { textInkRefresh(); }
static void cardMenuRefreshBand() { tickerBegin(); }
static void cardMenuRefreshText() {
  // The face first, so the reload lays the text out in it; for the
  // fields that left the face alone this sets what is already set.
  textSetFont(cfgCardTextFont(-1));
  cfgReloadText();
}
static void cardMenuRefreshInit() {
  aspectSet(cfgCardAspect(-1));
  insertSetMode(cfgCardInsert(-1));
  // The band sits in front of the aspect ratio, so it has to be laid
  // out again after those two.
  tickerBegin();
  cfgReload();
}

static int cardColorIndexOf(int kind) {
  const uint32_t rgb = cfgCardColor(cardMenuCard, kind) & 0xFFFFFFu;
  for (int i = 0; i < CARD_COLOR_COUNT; i++) {
    if (CARD_COLORS[i].rgb == rgb) return i;
  }
  return 0;   // not one of the seven: falls back to White
}
static void cardColorSetKind(int kind, int idx) {
  if (idx < 0 || idx >= CARD_COLOR_COUNT) return;
  cfgSetCardColor(cardMenuCard, kind, CARD_COLORS[idx].rgb);
  // The ticker ink and its lut are worked out in the band layout; the
  // other three sit in the cache textInkRefresh() fills.
  if (kind == CFG_COLOR_TICKER) cardMenuRefreshBand();
  else cardMenuRefreshInk();
}

static bool cmEnabledGet() { return cfgCardEnabled(cardMenuCard); }
static void cmEnabledSet(bool v) { cfgSetCardEnabled(cardMenuCard, v); }

static const char *cmTickerModeName(int i) {
  static const char *NAMES[] = {LANG_TICKMODE_OFF, LANG_TICKMODE_SOLID, LANG_TICKMODE_TRANSPARENT};
  return (i >= 0 && i < 3) ? NAMES[i] : "?";
}
static int cmTickerModeIdx() { return cfgCardTickerOf(cardMenuCard); }
static void cmTickerModeSet(int i) { cfgSetCardTickerOf(cardMenuCard, i); cardMenuRefreshBand(); }

static int cmTickerYGet() { return cfgCardTickerY(cardMenuCard); }
static void cmTickerYSet(int v) { cfgSetCardTickerY(cardMenuCard, v); cardMenuRefreshBand(); }

// SPEED NEEDS NO REFRESH: tickerFrame() reads it every picture, so the
// band changes pace or direction on the next one on its own, the same
// as the serial "tickerspeed" field.
static int cmTickerSpeedGet() { return cfgCardTickerSpeed(cardMenuCard); }
static void cmTickerSpeedSet(int v) { cfgSetCardTickerSpeed(cardMenuCard, v); }

// Which of the five fixed sizes the big faces (Inter, Doto) draw at;
// meaningless on the raster faces but harmless to cycle there too. Five
// named, individually baked sizes, not a range.
static const char *cmTickerSizeName(int i) {
  static const char *NAMES[] = {LANG_TICKSIZE_EXTRA_SMALL, LANG_TICKSIZE_SMALL, LANG_TICKSIZE_MEDIUM,
                                 LANG_TICKSIZE_LARGE, LANG_TICKSIZE_EXTRA_LARGE};
  return (i >= 0 && i < 5) ? NAMES[i] : "?";
}
static int cmTickerSizeIdx() { return cfgCardTickerSize(cardMenuCard); }
static void cmTickerSizeSet(int i) { cfgSetCardTickerSize(cardMenuCard, i); cardMenuRefreshBand(); }

// textFontName(), not a ticker-only table: both index the same FONTS[].
static int cmTickerFontIdx() { return cfgCardTickerFont(cardMenuCard); }
static void cmTickerFontSet(int i) { cfgSetCardTickerFont(cardMenuCard, i); cardMenuRefreshBand(); }

static int cmTickerColorIdx() { return cardColorIndexOf(CFG_COLOR_TICKER); }
static void cmTickerColorSet(int i) { cardColorSetKind(CFG_COLOR_TICKER, i); }

static int cmAspectIdx() { return cfgCardAspect(cardMenuCard); }
static void cmAspectSet(int i) { cfgSetCardAspect(cardMenuCard, i); cardMenuRefreshInit(); }

static bool cmShowIdGet() { return cfgCardShowId(cardMenuCard); }
static void cmShowIdSet(bool v) { cfgSetCardShowId(cardMenuCard, v); cardMenuRefreshText(); }
static int cmIdColorIdx() { return cardColorIndexOf(CFG_COLOR_ID); }
static void cmIdColorSet(int i) { cardColorSetKind(CFG_COLOR_ID, i); }

static bool cmShowSubGet() { return cfgCardShowSub(cardMenuCard); }
static void cmShowSubSet(bool v) { cfgSetCardShowSub(cardMenuCard, v); cardMenuRefreshText(); }
static int cmSubColorIdx() { return cardColorIndexOf(CFG_COLOR_SUB); }
static void cmSubColorSet(int i) { cardColorSetKind(CFG_COLOR_SUB, i); }

// No refresh needed: movingLineFrame() reads the flag once a picture.
static bool cmMovingGet() { return cfgCardMovingLine(cardMenuCard); }
static void cmMovingSet(bool v) { cfgSetCardMovingLine(cardMenuCard, v); }

static int cmInsertIdx() { return cfgCardInsert(cardMenuCard); }
static void cmInsertSet(int i) { cfgSetCardInsert(cardMenuCard, i); cardMenuRefreshInit(); }
static int cmInsertColorIdx() { return cardColorIndexOf(CFG_COLOR_INSERT); }
static void cmInsertColorSet(int i) { cardColorSetKind(CFG_COLOR_INSERT, i); }

static int cmTextFontIdx() { return cfgCardTextFont(cardMenuCard); }
static void cmTextFontSet(int i) { cfgSetCardTextFont(cardMenuCard, i); cardMenuRefreshText(); }

// G924's chroma bars and Card G's AP2 are not stored per card: one
// flag apiece, only ever read by the one card each belongs to.
static bool cmBarsGet() { return cfgG924ChromaBars(); }
static void cmBarsSet(bool v) { cfgSetG924ChromaBars(v); }
static bool cmAp2Get() { return cfgTestcardgAp2(); }
static void cmAp2Set(bool v) { cfgSetTestcardgAp2(v); }

// BBC Test Card F and W: unlike bars/AP2 above, IS per card, since the
// two need independent values (see CARD_CUSTOM_PHOTO in settings.cpp).
static bool cmCustomPhotoGet() { return cfgCardCustomPhoto(cardMenuCard); }
static void cmCustomPhotoSet(bool v) { cfgSetCardCustomPhoto(cardMenuCard, v); }

// The Sony PCM card's two switches, bits 0 and 1 of its option byte
// (see sonypcm.cpp). Read in the card's init(), so a change on the
// card in view asks for a reload, like the aspect ratio does.
static void cmPcmOptionSet(int bit, bool v) {
  int o = cfgCardOption(cardMenuCard);
  o = v ? (o | bit) : (o & ~bit);
  cfgSetCardOption(cardMenuCard, o);
  if (cardMenuCard == patternIndex) cfgReload();
}
static int cmPcmBitsIdx() { return (cfgCardOption(cardMenuCard) & 1) ? 1 : 0; }
static void cmPcmBitsSet(int i) { cmPcmOptionSet(1, i != 0); }
static const char *cmPcmBitsName(int i) { return i ? LANG_16_BIT : LANG_14_BIT; }
static bool cmPcmEmphGet() { return (cfgCardOption(cardMenuCard) & 2) != 0; }
static void cmPcmEmphSet(bool v) { cmPcmOptionSet(2, v); }
static bool cmPcmPicGet() { return (cfgCardOption(cardMenuCard) & 4) != 0; }
static void cmPcmPicSet(bool v) { cmPcmOptionSet(4, v); }

static const char *cmPcmTextGet() { return cfgPcmText(); }
// Ham PCM's quality, option bits 1..0: 0 HQ, 1 LQ, 2 Voice, 3 Voice narrow.
static int cmHamQualityIdx() { return cfgCardOption(cardMenuCard) & 3; }
static void cmHamQualitySet(int i) {
  int o = (cfgCardOption(cardMenuCard) & ~3) | (i & 3);
  cfgSetCardOption(cardMenuCard, o);
  if (cardMenuCard == patternIndex) cfgReload();
}
static const char *cmHamQualityName(int i) {
  return i == 1 ? LANG_HAM_LQ : i == 2 ? LANG_HAM_VOICE : i == 3 ? LANG_HAM_NARROW : LANG_HAM_HQ;
}
static void cmPcmTextSet(const char *s) { cfgSetPcmText(s); }
static const char *cmContest1Get() { return cfgContestText1(); }
static void cmContest1Set(const char *s) { cfgSetContestText1(s); }
static const char *cmContest2Get() { return cfgContestText2(); }
static void cmContest2Set(const char *s) { cfgSetContestText2(s); }
static int cmContestNrGet() { return cfgContestNumber(); }
static void cmContestNrSet(int v) { cfgSetContestNumber(v); }

typedef const char *(*CmTextGet)();
typedef void (*CmTextSet)(const char *);
typedef bool (*CmBoolGet)();
typedef void (*CmBoolSet)(bool);
typedef int (*CmIntGet)();
typedef void (*CmIntSet)(int);
typedef const char *(*CmName)(int);

// One shape for all four kinds this menu needs, rebuilt per card. A
// union, not four sets of fields side by side: a card's item is only
// ever one kind, so only one branch is live; 28 bytes instead of 60,
// which matters on an RP2350 this tight on RAM.
struct CardMenuItem {
  MenuKind kind;
  const char *label;
  union {
    struct {
      CmTextGet get;
      CmTextSet set;
      int maxLen;
    } text;
    struct {
      CmBoolGet get;
      CmBoolSet set;
    } toggle;
    struct {
      CmIntGet idx;
      CmIntSet set;
      int count;
      CmName name;
    } cycle;
    struct {
      CmIntGet get;
      CmIntSet set;
      int lo, hi, step;
    } slider;
  };
};

// 22: the busiest real card reaches 14 items (added Ticker size,
// 26 augustus 2026), comfortably below this.
static CardMenuItem cardMenuItems[22];
static int cardMenuItemN = 0;

// Plain function pointer syntax, not the CmTextGet/CmTextSet typedefs:
// the .ino build tool hoists a prototype, and a parameter typed with a
// name defined later in the file fails there.
static void cmAddText(const char *label, const char *(*g)(), void (*s)(const char *), int maxLen) {
  if (cardMenuItemN >= (int)(sizeof(cardMenuItems) / sizeof(cardMenuItems[0]))) return;
  CardMenuItem &it = cardMenuItems[cardMenuItemN++];
  memset(&it, 0, sizeof(it));
  it.kind = MENU_KIND_TEXT;
  it.label = label;
  it.text.get = g;
  it.text.set = s;
  it.text.maxLen = maxLen;
}
static void cmAddToggle(const char *label, bool (*g)(), void (*s)(bool)) {
  if (cardMenuItemN >= (int)(sizeof(cardMenuItems) / sizeof(cardMenuItems[0]))) return;
  CardMenuItem &it = cardMenuItems[cardMenuItemN++];
  memset(&it, 0, sizeof(it));
  it.kind = MENU_KIND_TOGGLE;
  it.label = label;
  it.toggle.get = g;
  it.toggle.set = s;
}
static void cmAddCycle(const char *label, int (*idx)(), void (*set)(int), int count,
                        const char *(*name)(int)) {
  if (cardMenuItemN >= (int)(sizeof(cardMenuItems) / sizeof(cardMenuItems[0]))) return;
  CardMenuItem &it = cardMenuItems[cardMenuItemN++];
  memset(&it, 0, sizeof(it));
  it.kind = MENU_KIND_CYCLE;
  it.label = label;
  it.cycle.idx = idx;
  it.cycle.set = set;
  it.cycle.count = count;
  it.cycle.name = name;
}
static void cmAddSlider(const char *label, int (*g)(), void (*s)(int), int lo, int hi, int step) {
  if (cardMenuItemN >= (int)(sizeof(cardMenuItems) / sizeof(cardMenuItems[0]))) return;
  CardMenuItem &it = cardMenuItems[cardMenuItemN++];
  memset(&it, 0, sizeof(it));
  it.kind = MENU_KIND_SLIDER;
  it.label = label;
  it.slider.get = g;
  it.slider.set = s;
  it.slider.lo = lo;
  it.slider.hi = hi;
  it.slider.step = step;
}

// Built once, when the menu opens, from cardMenuCard's traits
// (cfgCardTraits(), the same bits CardsTab.cs's row-hiding answers to);
// the card cannot change while this menu is open.
static void cardMenuBuild() {
  cardMenuItemN = 0;
  const uint8_t traits = cfgCardTraits(cardMenuCard);
  const bool hasId = (traits & CARD_TRAIT_ID) != 0;
  const bool hasSub = (traits & CARD_TRAIT_SUB) != 0;
  const bool hasLine = (traits & CARD_TRAIT_LINE) != 0;
  const bool hasBox = (traits & CARD_TRAIT_BOX) != 0;
  const bool hasContest = (traits & CARD_TRAIT_CONTEST) != 0;
  const bool hasG924 = (traits & CARD_TRAIT_G924) != 0;
  const bool hasCardG = (traits & CARD_TRAIT_CARDG) != 0;
  const bool fixedFont = (traits & CARD_TRAIT_FIXEDFONT) != 0;

  cmAddToggle(LANG_CARD_ENABLED, cmEnabledGet, cmEnabledSet);

  // A raw-signal card (pattern.h) has no ticker and no aspect ratio:
  // the same rows CardsTab.cs hides for own=5.
  const bool raw = PATTERNS[cardMenuCard]->rawSignal;
  if (!raw) {
    // Transparent does not fit on an uploaded photograph (see the same
    // guard on the "card.N.ticker" serial set, setSettingFromCommand()),
    // so a photo slot's own cycle stops one short of it.
    const int tickerModeCount = (customSlotOf(PATTERNS[cardMenuCard]) >= 0) ? 2 : 3;
    cmAddCycle(LANG_TICKER, cmTickerModeIdx, cmTickerModeSet, tickerModeCount, cmTickerModeName);
    cmAddSlider(LANG_TICKER_Y, cmTickerYGet, cmTickerYSet, 0, 576, 4);
    cmAddSlider(LANG_TICKER_SPEED, cmTickerSpeedGet, cmTickerSpeedSet, -8, 8, 1);
    cmAddCycle(LANG_TICKER_SIZE, cmTickerSizeIdx, cmTickerSizeSet, 5, cmTickerSizeName);
    cmAddCycle(LANG_TICKER_FONT, cmTickerFontIdx, cmTickerFontSet, FONT_COUNT, textFontName);
    cmAddCycle(LANG_TICKER_COLOUR, cmTickerColorIdx, cmTickerColorSet, CARD_COLOR_COUNT, cardColorName);

    cmAddCycle(LANG_ASPECT_2, cmAspectIdx, cmAspectSet, ASPECT_COUNT, aspectName);
  }

  if (hasId) {
    cmAddToggle(LANG_SHOW_FIRST_LINE, cmShowIdGet, cmShowIdSet);
    cmAddCycle(LANG_FIRST_LINE_COLOUR, cmIdColorIdx, cmIdColorSet, CARD_COLOR_COUNT, cardColorName);
  }
  if (hasSub) {
    cmAddToggle(LANG_SHOW_SECOND_LINE, cmShowSubGet, cmShowSubSet);
    cmAddCycle(LANG_SECOND_LINE_COLOUR, cmSubColorIdx, cmSubColorSet, CARD_COLOR_COUNT, cardColorName);
  }
  if (hasLine) {
    cmAddToggle(LANG_MOVING_LINE, cmMovingGet, cmMovingSet);
  }
  if (hasBox) {
    cmAddCycle(LANG_INSERT_BOXES, cmInsertIdx, cmInsertSet, INSERT_COUNT, insertModeName);
    cmAddCycle(LANG_INSERT_COLOUR, cmInsertColorIdx, cmInsertColorSet, CARD_COLOR_COUNT, cardColorName);
  }
  // A CARD WITH NO TEXT ANYWHERE has no use for the choice (the same
  // rule CardsTab.cs's own text font row follows), and neither does one
  // whose lettering is fixed, like Mixed bars's own Inter line.
  if ((hasId || hasSub || hasBox) && !fixedFont) {
    cmAddCycle(LANG_TEXT_FONT, cmTextFontIdx, cmTextFontSet, FONT_COUNT, textFontName);
  }
  if (hasG924) {
    cmAddToggle(LANG_CHROMA_BARS, cmBarsGet, cmBarsSet);
  }
  if (hasCardG) {
    cmAddToggle(LANG_AP2_VARIANT, cmAp2Get, cmAp2Set);
  }
  if (hasContest) {
    cmAddText(LANG_CONTEST_FIRST_LINE, cmContest1Get, cmContest1Set, 31);
    cmAddText(LANG_CONTEST_SECOND_LINE, cmContest2Get, cmContest2Set, 31);
    cmAddSlider(LANG_CONTEST_NUMBER, cmContestNrGet, cmContestNrSet, 0, 9999, 1);
  }
  // No CARD_TRAIT bit for this, that byte is full; only these two cards
  // have a photo window to swap, so checked by pattern identity instead.
  if (PATTERNS[cardMenuCard] == &PATTERN_TESTCARDF || PATTERNS[cardMenuCard] == &PATTERN_TESTCARDW) {
    cmAddToggle(LANG_CUSTOM_PHOTO, cmCustomPhotoGet, cmCustomPhotoSet);
  }
  if (PATTERNS[cardMenuCard] == &PATTERN_SONYPCM) {
    cmAddCycle(LANG_RESOLUTION, cmPcmBitsIdx, cmPcmBitsSet, 2, cmPcmBitsName);
    cmAddToggle(LANG_PRE_EMPHASIS, cmPcmEmphGet, cmPcmEmphSet);
    cmAddToggle(LANG_CTRL_IN_PICTURE, cmPcmPicGet, cmPcmPicSet);
  }
  if (PATTERNS[cardMenuCard] == &PATTERN_PCMAUDIO) {
    cmAddCycle(LANG_HAM_QUALITY, cmHamQualityIdx, cmHamQualitySet, 4, cmHamQualityName);
    cmAddText(LANG_PCM_TEXT, cmPcmTextGet, cmPcmTextSet, PCM_TEXT_MAX - 1);
  }
}

// One past the last real item is Close, the same convention menuCloseIndex()
// uses for the System menu.
static int cardMenuTotal() { return cardMenuItemN + 1; }

static void cardMenuOpenItem() {
  if (menuIndex < 0 || menuIndex >= cardMenuItemN) {   // Close
    menuOpen = false;
    return;
  }
  const CardMenuItem &it = cardMenuItems[menuIndex];
  if (it.kind == MENU_KIND_TEXT) {
    menuEditMaxLen = it.text.maxLen;
    const char *cur = it.text.get();
    int i = 0;
    for (; cur[i] && i < menuEditMaxLen; i++) menuEditBuf[i] = cur[i];
    for (; i < menuEditMaxLen; i++) menuEditBuf[i] = ' ';
    menuEditBuf[menuEditMaxLen] = 0;
    menuEditCursor = 0;
    menuMode = MENU_MODE_EDIT;
  } else if (it.kind == MENU_KIND_TOGGLE) {
    it.toggle.set(!it.toggle.get());
  } else if (it.kind == MENU_KIND_CYCLE) {
    if (it.cycle.count > 0) it.cycle.set((it.cycle.idx() + 1) % it.cycle.count);
  } else if (it.kind == MENU_KIND_SLIDER) {
    menuMode = MENU_MODE_ADJUST;
  }
}

static void cardMenuCommitEdit() {
  if (menuIndex >= 0 && menuIndex < cardMenuItemN) {
    const CardMenuItem &it = cardMenuItems[menuIndex];
    int len = menuEditMaxLen;
    while (len > 0 && menuEditBuf[len - 1] == ' ') len--;
    menuEditBuf[len] = 0;
    it.text.set(menuEditBuf);
  }
  menuMode = MENU_MODE_NAV;
}

static void cardMenuTurnNav(int turned) {
  const int total = cardMenuTotal();
  menuIndex = ((menuIndex + turned) % total + total) % total;
}

static void cardMenuTurnAdjust(int turned) {
  if (menuIndex < 0 || menuIndex >= cardMenuItemN) return;
  const CardMenuItem &it = cardMenuItems[menuIndex];
  int v = it.slider.get() + turned * it.slider.step;
  if (v < it.slider.lo) v = it.slider.lo;
  if (v > it.slider.hi) v = it.slider.hi;
  it.slider.set(v);
}

// Only called from menuPress()'s own NAV branch: EDIT and ADJUST do
// not differ between the two menus, only opening an item does.
static void cardMenuPress() { cardMenuOpenItem(); }

// Same shape as updateMenuDisplay() below, kept separate since what it
// reads from differs at nearly every line.
static void cardMenuUpdateDisplay() {
  char label[24], value[TICKER_MAX_CHARS + 1], hint[40], count[16];
  int cursorIndex = -1;
  bool cursorVisible = false;
  bool leftAlign = (menuMode == MENU_MODE_EDIT);
  bool barOn = false;
  int barLo = 0, barHi = 0, barValue = 0;
  const bool isClose = (menuIndex < 0 || menuIndex >= cardMenuItemN);

  if (isClose) {
    label[0] = 0;
    snprintf(value, sizeof(value), LANG_CLOSE_MENU);
    hint[0] = 0;
  } else {
    const CardMenuItem &it = cardMenuItems[menuIndex];
    snprintf(label, sizeof(label), "%s", it.label);
    if (menuMode == MENU_MODE_EDIT) {
      snprintf(value, sizeof(value), "%s", menuEditBuf);
      cursorIndex = menuEditCursor;
      cursorVisible = (millis() / 500) % 2 == 0;
      snprintf(hint, sizeof(hint), LANG_TURN_LETTER_PRESS_NEXT);
    } else if (menuMode == MENU_MODE_ADJUST) {
      snprintf(value, sizeof(value), "%d", it.slider.get());
      barOn = true;
      barLo = it.slider.lo;
      barHi = it.slider.hi;
      barValue = it.slider.get();
      snprintf(hint, sizeof(hint), LANG_TURN_VALUE_PRESS_HOLD);
    } else {
      switch (it.kind) {
        case MENU_KIND_TEXT: {
          const char *stored = it.text.get();
          snprintf(value, sizeof(value), "%s", stored);
          // Scrolls if it does not fit, same clock as the System menu's
          // own text preview.
          const int len = (int)strlen(stored);
          if (len > 0) {
            const uint32_t cyc = (uint32_t)len + MENU_TEXT_SCROLL_END_PAUSE;
            uint32_t t = (millis() / MENU_TEXT_SCROLL_STEP_MS) % cyc;
            if (t >= (uint32_t)len) t = (uint32_t)len - 1;
            cursorIndex = (int)t;
          }
          leftAlign = true;
          snprintf(hint, sizeof(hint), LANG_PRESS_EDIT_HOLD_CLOSE);
          break;
        }
        case MENU_KIND_TOGGLE:
          snprintf(value, sizeof(value), "%s", it.toggle.get() ? LANG_ENABLED : LANG_DISABLED);
          snprintf(hint, sizeof(hint), LANG_PRESS_TOGGLE_HOLD_CLOSE);
          break;
        case MENU_KIND_CYCLE:
          snprintf(value, sizeof(value), "%s", it.cycle.name(it.cycle.idx()));
          snprintf(hint, sizeof(hint), LANG_PRESS_NEXT_HOLD_CLOSE);
          break;
        case MENU_KIND_SLIDER:
          snprintf(value, sizeof(value), "%d", it.slider.get());
          barOn = true;
          barLo = it.slider.lo;
          barHi = it.slider.hi;
          barValue = it.slider.get();
          snprintf(hint, sizeof(hint), LANG_PRESS_ADJUST_HOLD_CLOSE);
          break;
        default:
          value[0] = 0;
          hint[0] = 0;
          break;
      }
    }
  }
  snprintf(count, sizeof(count), "%d/%d", menuIndex + 1, cardMenuTotal());
  displaySetMenu(label, value, hint, count, leftAlign, cursorIndex, cursorVisible);
  displaySetMenuBar(barOn, barLo, barHi, barValue);
}

// Enters whatever the item at menuIndex offers: a toggle flips and a
// cycle advances right here, on the spot, because both are one immediate
// action and neither needs a submode of its own. A text field, the
// slider and a control switch menuMode instead, because those need
// several turns before there is anything to keep.
static void menuOpenItem() {
  const MenuKind kind = menuKindAt(menuIndex);
  if (kind == MENU_KIND_TEXT) {
    const MenuTextField &f = MENU_TEXT_FIELDS[menuIndex];
    menuEditMaxLen = f.maxLen;
    // Space padded, not NUL (strncpy()'s way): the buffer is reused
    // field to field, and every position must be a real MENU_CHARS
    // character so the cursor can go anywhere in range.
    const char *cur = f.get();
    int i = 0;
    for (; cur[i] && i < menuEditMaxLen; i++) menuEditBuf[i] = cur[i];
    for (; i < menuEditMaxLen; i++) menuEditBuf[i] = ' ';
    menuEditBuf[menuEditMaxLen] = 0;
    menuEditCursor = 0;
    menuMode = MENU_MODE_EDIT;
  } else if (kind == MENU_KIND_TOGGLE) {
    const MenuToggle &t = MENU_TOGGLES[menuToggleIndex(menuIndex)];
    t.set(!t.get());
  } else if (kind == MENU_KIND_CYCLE) {
    const MenuCycle &c = MENU_CYCLES[menuCycleIndexOf(menuIndex)];
    const int n = c.count();
    if (n > 0) c.set((c.index() + 1) % n);
  } else if (kind == MENU_KIND_SLIDER) {
    menuMode = MENU_MODE_ADJUST;
  } else if (kind == MENU_KIND_PARAM) {
    advParamSelect(menuParamIndexOf(menuIndex));
    menuMode = MENU_MODE_ADJUST;
  } else if (kind == MENU_KIND_DATETIME) {
    ClockTime t;
    clockGet(&t);
    menuDtYear = t.year;
    menuDtMonth = t.month;
    menuDtDay = t.day;
    menuDtHour = t.hour;
    menuDtMin = t.min;
    menuDtSec = t.sec;
    menuDtField = 0;
    menuMode = MENU_MODE_ADJUST;
  } else if (kind == MENU_KIND_CARDENABLE) {
    // Not reset: keeps whichever card was last looked at across menu
    // visits, only snapped onto a visible one if it is not one already.
    if (!menuEnableCardVisible(menuEnableCardIdx)) {
      menuEnableCardIdx = menuEnableCardStep(menuEnableCardIdx, 1);
    }
    menuMode = MENU_MODE_ADJUST;
  } else {
    menuOpen = false;
  }
}

// Trailing spaces trimmed before the field is written: the buffer is
// always menuEditMaxLen, space padded (see menuOpenItem()).
static void menuCommitEdit() {
  const MenuTextField &f = MENU_TEXT_FIELDS[menuIndex];
  int len = menuEditMaxLen;
  while (len > 0 && menuEditBuf[len - 1] == ' ') len--;
  menuEditBuf[len] = 0;
  f.set(menuEditBuf);
  menuMode = MENU_MODE_NAV;
}

static void menuTurn(int turned) {
  if (menuMode == MENU_MODE_NAV) {
    if (menuIsCard) {
      cardMenuTurnNav(turned);
      return;
    }
    const int total = menuTotalCount();
    menuIndex = ((menuIndex + turned) % total + total) % total;
  } else if (menuMode == MENU_MODE_EDIT) {
    // Every position is already a real character (see menuOpenItem()),
    // so this reads and writes menuEditBuf[menuEditCursor] directly;
    // shared with the card menu, a letter is a letter either way.
    const char c = menuEditBuf[menuEditCursor];
    const int idx = ((menuCharIndex(c) + turned) % MENU_CHARS_N + MENU_CHARS_N) % MENU_CHARS_N;
    menuEditBuf[menuEditCursor] = MENU_CHARS[idx];
  } else {   // MENU_MODE_ADJUST
    if (menuIsCard) {
      cardMenuTurnAdjust(turned);
      return;
    }
    if (menuKindAt(menuIndex) == MENU_KIND_SLIDER) {
      int v = cfgDisplayBrightness() + turned * 5;
      if (v < 0) v = 0;
      if (v > 100) v = 100;
      cfgSetDisplayBrightness(v);
      displaySetBrightness(v);   // live, the same as the setting's own value is meant to look
    } else if (menuKindAt(menuIndex) == MENU_KIND_DATETIME) {
      menuDtTurn(turned);
    } else if (menuKindAt(menuIndex) == MENU_KIND_CARDENABLE) {
      // Walks rather than a plain modulo: steps one visible card at a
      // time, skipping empty photograph slots; see menuEnableCardVisible().
      const int n = (turned > 0) ? turned : -turned;
      const int dir = (turned > 0) ? 1 : -1;
      for (int i = 0; i < n; i++) menuEnableCardIdx = menuEnableCardStep(menuEnableCardIdx, dir);
    } else {
      const int n = (turned > 0) ? turned : -turned;
      for (int i = 0; i < n; i++) advParamStep(turned > 0 ? 1 : -1);
    }
  }
}

static void menuPress() {
  if (menuMode == MENU_MODE_NAV) {
    if (menuIsCard) {
      cardMenuPress();
      return;
    }
    menuOpenItem();
  } else if (menuMode == MENU_MODE_EDIT) {
    if (menuEditCursor + 1 < menuEditMaxLen) menuEditCursor++;
  } else if (!menuIsCard && menuKindAt(menuIndex) == MENU_KIND_DATETIME) {
    // One field at a time; past the last one (seconds) it reaches the
    // clock, the same point menuCommitEdit() writes a text field back.
    if (menuDtField + 1 < 6) {
      menuDtField++;
    } else {
      clockSet(menuDtYear, menuDtMonth, menuDtDay, menuDtHour, menuDtMin, menuDtSec);
      menuMode = MENU_MODE_NAV;
    }
  } else if (!menuIsCard && menuKindAt(menuIndex) == MENU_KIND_CARDENABLE) {
    // Toggles and stays, unlike other ADJUST items: turning only picks
    // the card, so a press is the one gesture that changes anything,
    // letting several cards be fixed in one visit.
    cfgSetCardEnabled(menuEnableCardIdx, !cfgCardEnabled(menuEnableCardIdx));
  } else {   // MENU_MODE_ADJUST
    menuMode = MENU_MODE_NAV;   // the value is already live; nothing left to confirm
  }
}

// Long press goes back one level rather than out of the menu from
// wherever it lands: EDIT and ADJUST return to the list (EDIT saving
// what was typed), and the list is where a long press finally closes
// the menu.
static void menuLongPress() {
  if (menuMode == MENU_MODE_EDIT) {
    if (menuIsCard) {
      cardMenuCommitEdit();
    } else {
      menuCommitEdit();
    }
  } else if (menuMode == MENU_MODE_ADJUST) {
    // Holding past any field saves what is staged so far, wherever in
    // the six fields it lands: a long press means "I am done", not
    // "throw this away".
    if (!menuIsCard && menuKindAt(menuIndex) == MENU_KIND_DATETIME) {
      clockSet(menuDtYear, menuDtMonth, menuDtDay, menuDtHour, menuDtMin, menuDtSec);
    }
    menuMode = MENU_MODE_NAV;
  } else {
    menuOpen = false;
    menuMode = MENU_MODE_NAV;
  }
}

// Composes the panel's label/value/hint/count for whatever menuIndex
// and menuMode currently are, and hands them to display.cpp, whole
// field and cursor index each time; layoutValueText() there measures
// the real per-glyph widths and finds the smallest scroll that still
// shows the cursor's column. Called every picture while menuOpen; the
// strcmp guards inside display.cpp keep that cheap.
static void updateMenuDisplay() {
  if (menuIsCard) {
    cardMenuUpdateDisplay();
    return;
  }
  char label[24], value[TICKER_MAX_CHARS + 1], hint[40], count[16];
  const MenuKind kind = menuKindAt(menuIndex);
  int cursorIndex = -1;
  bool cursorVisible = false;
  // Left aligned while editing, and for the scrolling NAV-mode text
  // preview below (a sliding window's width changes step to step, so
  // centring it would shift it sideways every time); centred everywhere
  // else, where a short word or number reads better in the middle.
  bool leftAlign = (menuMode == MENU_MODE_EDIT);
  // A bargraph beside the number for a slider or analogue control, in
  // both NAV and ADJUST: value is unchanged, this only hands
  // display.cpp the range it needs to draw the bar.
  bool barOn = false;
  int barLo = 0, barHi = 0, barValue = 0;

  if (menuMode == MENU_MODE_EDIT) {
    snprintf(label, sizeof(label), "%s", MENU_TEXT_FIELDS[menuIndex].label);
    // The whole field, not a pre-cut window: layoutValueText() in
    // display.cpp measures the real glyph widths itself.
    snprintf(value, sizeof(value), "%s", menuEditBuf);
    cursorIndex = menuEditCursor;
    cursorVisible = (millis() / 500) % 2 == 0;
    snprintf(hint, sizeof(hint), LANG_TURN_LETTER_PRESS_NEXT);
    snprintf(count, sizeof(count), "%d/%d", menuEditCursor + 1, menuEditMaxLen);
  } else if (menuMode == MENU_MODE_ADJUST) {
    if (kind == MENU_KIND_SLIDER) {
      // "Display brightness", not "BRIGHTNESS": this is the panel's own
      // backlight (MENU_BRIGHTNESS_INDEX), not a picture control, and
      // the same sentence case as UiText.GroupBrightness on the PC side.
      snprintf(label, sizeof(label), LANG_DISPLAY_BRIGHTNESS);
      snprintf(value, sizeof(value), "%d", cfgDisplayBrightness());
      barOn = true;
      barLo = 0;
      barHi = 100;
      barValue = cfgDisplayBrightness();
      snprintf(hint, sizeof(hint), LANG_TURN_VALUE_PRESS_HOLD);
    } else if (kind == MENU_KIND_DATETIME) {
      snprintf(label, sizeof(label), LANG_DATE_AND_TIME);
      menuDtFormat(value, sizeof(value), menuDtField, menuDtYear, menuDtMonth, menuDtDay,
                   menuDtHour, menuDtMin, menuDtSec);
      snprintf(hint, sizeof(hint), LANG_TURN_VALUE_PRESS_NEXT);
    } else if (kind == MENU_KIND_CARDENABLE) {
      snprintf(label, sizeof(label), LANG_ENABLE_CARD);
      menuEnableCardFormat(value, sizeof(value));
      cursorIndex = menuScrollCursor(value);
      leftAlign = true;
      snprintf(hint, sizeof(hint), LANG_TURN_CARD_PRESS_TOGGLE);
    } else {
      const int pi = menuParamIndexOf(menuIndex);
      snprintf(label, sizeof(label), "%s", advParamName(pi));
      snprintf(value, sizeof(value), "%d", advParamValue(pi));
      barOn = true;
      barLo = advParamMin(pi);
      barHi = advParamMax(pi);
      barValue = advParamValue(pi);
      snprintf(hint, sizeof(hint), LANG_TURN_VALUE_PRESS_HOLD);
    }
    if (kind == MENU_KIND_CARDENABLE) {
      snprintf(count, sizeof(count), "%d/%d", menuEnableCardVisibleRank(menuEnableCardIdx),
               menuEnableCardVisibleCount());
    } else {
      snprintf(count, sizeof(count), "%d/%d", menuIndex + 1, menuTotalCount());
    }
  } else {
    switch (kind) {
      case MENU_KIND_TEXT: {
        snprintf(label, sizeof(label), "%s", MENU_TEXT_FIELDS[menuIndex].label);
        const char *stored = MENU_TEXT_FIELDS[menuIndex].get();
        snprintf(value, sizeof(value), "%s", stored);
        // Scrolls if it does not fit: the "cursor" here is a clock
        // sweeping start to end, held briefly at the end.
        const int len = (int)strlen(stored);
        if (len > 0) {
          const uint32_t cycle = (uint32_t)len + MENU_TEXT_SCROLL_END_PAUSE;
          uint32_t t = (millis() / MENU_TEXT_SCROLL_STEP_MS) % cycle;
          if (t >= (uint32_t)len) t = (uint32_t)len - 1;
          cursorIndex = (int)t;
        }
        leftAlign = true;
        snprintf(hint, sizeof(hint), LANG_PRESS_EDIT_HOLD_CLOSE);
        break;
      }
      case MENU_KIND_TOGGLE: {
        const MenuToggle &t = MENU_TOGGLES[menuToggleIndex(menuIndex)];
        snprintf(label, sizeof(label), "%s", t.label);
        // "Enabled"/"Disabled", not "ON"/"OFF": same word in both menus,
        // see the card menu's own MENU_KIND_TOGGLE case.
        snprintf(value, sizeof(value), "%s", t.get() ? LANG_ENABLED : LANG_DISABLED);
        snprintf(hint, sizeof(hint), LANG_PRESS_TOGGLE_HOLD_CLOSE);
        break;
      }
      case MENU_KIND_CYCLE: {
        const MenuCycle &c = MENU_CYCLES[menuCycleIndexOf(menuIndex)];
        snprintf(label, sizeof(label), "%s", c.label);
        snprintf(value, sizeof(value), "%s", c.name(c.index()));
        snprintf(hint, sizeof(hint), LANG_PRESS_NEXT_HOLD_CLOSE);
        break;
      }
      case MENU_KIND_SLIDER:
        snprintf(label, sizeof(label), LANG_DISPLAY_BRIGHTNESS);
        snprintf(value, sizeof(value), "%d", cfgDisplayBrightness());
        snprintf(hint, sizeof(hint), LANG_PRESS_ADJUST_HOLD_CLOSE);
        barOn = true;
        barLo = 0;
        barHi = 100;
        barValue = cfgDisplayBrightness();
        break;
      case MENU_KIND_PARAM: {
        const int pi = menuParamIndexOf(menuIndex);
        snprintf(label, sizeof(label), "%s", advParamName(pi));
        snprintf(value, sizeof(value), "%d", advParamValue(pi));
        snprintf(hint, sizeof(hint), LANG_PRESS_ADJUST_HOLD_CLOSE);
        barOn = true;
        barLo = advParamMin(pi);
        barHi = advParamMax(pi);
        barValue = advParamValue(pi);
        break;
      }
      case MENU_KIND_DATETIME: {
        // The running clock, read fresh every call: nothing is staged
        // until the item is opened (see menuOpenItem()), so looking at
        // it shows the real time, ticking.
        snprintf(label, sizeof(label), LANG_DATE_AND_TIME);
        ClockTime t;
        clockGet(&t);
        menuDtFormat(value, sizeof(value), -1, t.year, t.month, t.day, t.hour, t.min, t.sec);
        snprintf(hint, sizeof(hint), LANG_PRESS_SET_HOLD_CLOSE);
        break;
      }
      case MENU_KIND_CARDENABLE: {
        snprintf(label, sizeof(label), LANG_ENABLE_CARD);
        menuEnableCardFormat(value, sizeof(value));
        cursorIndex = menuScrollCursor(value);
        leftAlign = true;
        snprintf(hint, sizeof(hint), LANG_PRESS_PICK_HOLD_CLOSE);
        break;
      }
      default:   // MENU_KIND_CLOSE
        // "Close menu" alone says enough that the label and hint would
        // only repeat it; both blank, the position count stays.
        label[0] = 0;
        snprintf(value, sizeof(value), LANG_CLOSE_MENU);
        hint[0] = 0;
        break;
    }
    if (kind == MENU_KIND_CARDENABLE) {
      snprintf(count, sizeof(count), "%d/%d", menuEnableCardVisibleRank(menuEnableCardIdx),
               menuEnableCardVisibleCount());
    } else {
      snprintf(count, sizeof(count), "%d/%d", menuIndex + 1, menuTotalCount());
    }
  }
  displaySetMenu(label, value, hint, count, leftAlign, cursorIndex, cursorVisible);
  displaySetMenuBar(barOn, barLo, barHi, barValue);
}

// The four lines beside the miniature on the status display. Composed
// here, not in display.cpp, which only draws; unchanged lines are not
// re-sent, so this may run every picture.
static void updateDisplayLines() {
  if (!displayOn()) return;

  // The menu takes the whole panel, like the candidate offer below.
  if (menuOpen) {
    // The candidate screen has priority over the menu in
    // displayFrameHook(); left standing it froze over a live,
    // invisible menu when the card menu opened off a turned hold.
    displaySetCandidate(nullptr, nullptr);
    updateMenuDisplay();
    return;
  }
  displaySetMenu(nullptr, nullptr, nullptr, nullptr, false, -1, false);
  displaySetMenuBar(false, 0, 0, 0);

  char buf[48];

  // The name without what is in brackets ("PM5544 G913 (monoscope)" is
  // wider than the text area); an uploadable card shows its file name
  // instead of "Custom 7", which says nothing about what is on screen.
  const Pattern *shown = (encoderCandidate >= 0) ? PATTERNS[encoderCandidate] : activePattern;
  const char *fileName = customFileName(shown);
  const char *name = fileName ? fileName : shown->name;
  const char *bracket = strstr(name, " (");
  int nameLen = bracket ? (int)(bracket - name) : (int)strlen(name);
  snprintf(buf, sizeof(buf), "%.*s", nameLen, name);

  // What the knob is offering takes the whole panel while a choice is
  // pending, see displaySetCandidate(); the picture itself is unchanged.
  if (encoderCandidate >= 0) {
    char candCount[16];
    snprintf(candCount, sizeof(candCount), "%d/%d", enabledPatternRank(encoderCandidate),
             enabledPatternCount());
    displaySetCandidate(buf, candCount);
    return;
  }
  displaySetCandidate(nullptr, nullptr);

  displaySetLine(0, buf);

  // The stored texts, not the shown ones: with the identification off
  // the panel still says what would be shown, in dark grey. The count
  // is of enabled cards, the same rule p/P and the knob step by.
  char count[16], id[2 * CFG_TEXT_MAX + 4];
  snprintf(count, sizeof(count), "%d/%d", enabledPatternRank(patternIndex), enabledPatternCount());
  snprintf(id, sizeof(id), "%s / %s", cfgTextId(), cfgTextSub());
  // Dark grey when it is not on the screen, and that is two cases: the
  // card has it switched off, or the card has no place for it at all.
  displaySetInfo(count, id, !cfgShowId() || !cfgCardHasId());

  // The badges: each carries something not visible in the picture.
  displaySetBadge(0, LANG_RGB, !activePattern->mono);

  // TXT and ITS follow the VBI door (key v): both are pixel data on the
  // blanking lines, absent at the output with that door shut. A raw-
  // signal card (pattern.h) sends none of the three, whatever is set.
  const bool vbi = advVbiOpen();
  const bool raw = activePattern->rawSignal;
  displaySetBadge(1, LANG_TXT, teletextOn() && vbi && !raw);

  // WSS does not follow the VBI door: the encoder makes it itself on
  // line 23, outside the VBI's line 7-22.
  displaySetBadge(2, LANG_WSS, aspectGet() != ASPECT_OFF && !raw);

  displaySetBadge(3, LANG_ITS, itsEnabled() && vbi && !raw);

  // CLK is lit when the insert boxes carry the clock.
  const int insert = insertGetMode();
  const bool clock = insertInUse() &&
                     (insert == INSERT_TIME || insert == INSERT_DATE_TIME);
  displaySetBadge(4, LANG_CLK, clock);

  displaySetBadge(5, LANG_SHOW, slideshowOn);

  // USB is lit on DTR, the same flag TinyUSB uses for "connected".
  displaySetBadge(6, LANG_USB, (bool)Serial);
}

// Called once per frame from the producer loop (see video.h).
void videoFrameHook() {
  // Once a frame, not once a row as before: 625 times cheaper, still
  // feeds the watchdog every 40 ms against an 8 s timeout.
  videoFeedWatchdog();

  // The row-counter wrap guard, first half; see VIDEO_RENORM_STEP in
  // videoloop.h. Core1 answers within a row (its videoLoadLine runs
  // every 64 us even parked), so the request is always consumed long
  // before any videoSuspend()/videoResume() this same core could
  // start.
  if (videoConsumedCount >= 0x80000000u && !videoRenormRequest) {
    videoProducedCount -= VIDEO_RENORM_STEP;
    videoRenormRequest = true;
  }

  // The activity LED: one toggle a frame, so it tracks whether fresh
  // pictures are actually being produced rather than just a card
  // choice. 12.5 Hz (a full on/off cycle every two frames), the fastest
  // this hook itself runs; not once a row, which would look solid on.
  // It freezes rather than going dark during a blocking flash write
  // (upload/erase suspend the video and with it loop()'s own call into
  // this hook), which reads as "no fresh picture" all the same.
  static bool ledOn = false;
  ledOn = !ledOn;
  digitalWrite(PIN_ACTIVITY_LED, ledOn);

  // Once a picture, not once a field, or vertical strokes in the text
  // comb at 25 Hz. The moving line is the exception: called once a
  // picture too, but hands each field its own position, since a moving
  // shape in two fields is two moments in time; see movingline.cpp.
  tickerFrame();
  movingLineFrame();

  // The encoder: phases fire their own interrupt (encoder.cpp), and
  // steps/push are taken once a picture, since a card change belongs
  // between two pictures.
  {
    const int turned = encoderTakeSteps();
    const bool pressed = encoderTakePush();
    const bool released = encoderTakeRelease();
    const bool longPressed = encoderTakeLongPress();

    if (menuOpen) {
      // The menu owns the knob while open; the candidate branch below
      // does not run, so the panel never has two full-screen modes
      // competing for it.
      if (longPressed) {
        encoderFlush();
        menuHeldForLongPress = true;
        menuLongPress();
        menuTurnGuardArmedMs = millis();
      } else {
        // Wrap-safe subtraction: millis() wraps every ~49.7 days, which
        // a bare deadline comparison would misread after every wrap.
        if (turned && millis() - menuTurnGuardArmedMs >= MENU_TURN_GUARD_MS) menuTurn(turned);
        if (pressed) {
          // No menuPress() here: it used to run on this same press-down
          // edge that every long press also starts with, so closing a
          // toggle by holding the knob flipped it first. Moved to the
          // release below, gated on menuHeldForLongPress.
          encoderFlush();
          menuHeldForLongPress = false;
          menuTurnGuardArmedMs = millis();
        }
        if (released) {
          // Runs here, only for a release that was not the tail end of
          // a long press: a confirmed short click.
          encoderFlush();
          if (!menuHeldForLongPress) menuPress();
          menuTurnGuardArmedMs = millis();
        }
      }
      if (turned || pressed || released || longPressed) menuLastMs = millis();
      if (menuOpen && millis() - menuLastMs > MENU_TIMEOUT_MS) {
        // Commits whatever was staged, the same as a long press would,
        // instead of silently dropping it.
        if (menuMode == MENU_MODE_EDIT) {
          if (menuIsCard) {
            cardMenuCommitEdit();
          } else {
            menuCommitEdit();
          }
        } else if (menuMode == MENU_MODE_ADJUST && !menuIsCard &&
                   menuKindAt(menuIndex) == MENU_KIND_DATETIME) {
          clockSet(menuDtYear, menuDtMonth, menuDtDay, menuDtHour, menuDtMin, menuDtSec);
        }
        menuOpen = false;
        menuMode = MENU_MODE_NAV;
        report(LANG_MENU_TIMED_OUT_CLOSED);
      }
      if (!menuOpen && cfgDirty()) {
        // The menu's own close is the command, same as key *: whatever
        // changed while it was open, timed out or not, is kept.
        if (saveNow()) report(LANG_STORAGE_SAVED);
      }
    } else if (longPressed && encoderCandidate < 0) {
      // Known quirk, not yet resolved: holding the button while a
      // candidate is offered selects it, and if the hold continues also
      // opens the menu.
      menuOpen = true;
      menuIsCard = false;
      menuIndex = 0;
      menuMode = MENU_MODE_NAV;
      menuLastMs = millis();
      cardMenuPendingOpen = false;
      // Marked here too: the knob is still held, so the release ending
      // this hold lands in the released branch below.
      menuHeldForLongPress = true;
      menuTurnGuardArmedMs = millis();   // the opening hold can nudge the shaft
      report(LANG_MENU_OPEN);
    } else {
      if (turned) {
        // Turning while the knob is held is browsing, not the click
        // that was going to open the card menu on release; without
        // this the release of a long, turned hold opened the menu.
        cardMenuPendingOpen = false;
        // Starts from what is showing, so the first click offers the
        // neighbour of the current card and not of some earlier choice.
        int at = (encoderCandidate >= 0) ? encoderCandidate : patternIndex;
        const int step = (turned > 0) ? 1 : -1;
        for (int n = 0; n < (turned > 0 ? turned : -turned); n++) at = nextPattern(at, step);
        encoderCandidate = (at == patternIndex) ? -1 : at;
        encoderCandidateLastMs = millis();
        report(LANG_KNOB_D_D_S_PRESS,
               at + 1, PATTERN_COUNT, PATTERNS[at]->name);
      }
      if (pressed) {
        // With a candidate offered, a press switches to it right away.
        // Otherwise only the decision is made here; menuOpen waits for
        // the release below, so a press that turns into a long press
        // never opens the card menu.
        if (encoderCandidate < 0) {
          cardMenuPendingOpen = true;
        } else if (requestedPattern < 0) {
          requestedPattern = encoderCandidate;
          encoderCandidate = -1;
        }
      }
      if (released && cardMenuPendingOpen) {
        cardMenuPendingOpen = false;
        encoderCandidate = -1;
        menuOpen = true;
        menuIsCard = true;
        cardMenuCard = patternIndex;
        cardMenuBuild();
        menuIndex = 0;
        menuMode = MENU_MODE_NAV;
        menuLastMs = millis();
        menuTurnGuardArmedMs = millis();   // the opening release can nudge the shaft
        report(LANG_MENU_CARD_OPEN);
      }
      // ABANDONED: nobody turned or pressed for CANDIDATE_TIMEOUT_MS, so
      // the offer is withdrawn on its own and the panel returns to the
      // working layout by itself.
      if (encoderCandidate >= 0 && millis() - encoderCandidateLastMs > CANDIDATE_TIMEOUT_MS) {
        encoderCandidate = -1;
        report(LANG_KNOB_CANDIDATE_TIMED_OUT_BACK, patternIndex + 1, PATTERN_COUNT);
      }
    }
  }

  // A switch reports once its copy is done and no more rows are late,
  // so the figure covers the catching up too.
  if (switchRunning && !romCopyPending()) {
    VideoStats s;
    videoGetStats(&s);
    if (s.starves == switchLastStarves) {
      reportSwitch();
    } else {
      switchLastStarves = s.starves;
    }
  }

  // Anything that moves (the clock) updates here, between two pictures,
  // so half an old picture and half a new one never appear together.
  if (activePattern->tick) activePattern->tick();

  // The teletext header clock: only recomputes packet X/0's waveform
  // when the second ticks over.
  teletextTick();

  // Not while a card change is being copied in: composing and drawing
  // the status display costs about 75 us of the margin.
  if (!romCopyPending()) {
    updateDisplayLines();
    displayFrameHook();
  }

  // Several characters read per picture, bounded so a flood of input
  // cannot hold up row production.
  if (Serial) {
    // Said once when something becomes unsaved: only key * writes to
    // flash, and a change never written is silently lost on power cut.
    const bool wasDirty = cfgDirty();
    for (int i = 0; i < 32 && Serial.available(); i++) handleSerialChar(Serial.read());
    if (!wasDirty && cfgDirty()) report(LANG_STORAGE_CHANGED_KEY_WRITES_IT);
  }
  sflFinish();

  encoderConfigWatch();

  // The hourly RTC re-anchor costs an I2C read on this core, so only
  // with the row stock full enough, the same rule as the repair in
  // encoderConfigWatch().
  if ((int32_t)(videoProducedCount - videoConsumedCount) >= VIDEO_ROM_COPY_KEEP_ROWS) {
    clockService();
  }

  // If half a line is left hanging, because the terminal sends no
  // Enter, drop it here after all.
  if (cmdLen >= 0 && millis() - cmdLastMs > CMD_TIMEOUT_MS) cmdAbort(LANG_TIMED_OUT);

  // A dump with no spare slot at all for ten seconds is broken off with
  // a message rather than left hanging in silence.
  if (dumpActive && !dumpEndState && millis() - dumpLastProgressMs > 10000) {
    dumpEndState = 2;
    dumpEndWhy = LANG_NO_SPARE_TIME_TO_RENDER;
  }
  if (dumpActive && dumpEndState) cardDumpFinish();

  // Not while a dump runs, or the PC would get a picture half the old
  // layout and half the new; the flags stay up and are answered right
  // after.
  if (dumpActive) {
  } else if (cfgReloadNeeded() && millis() - lastSwitchMs >= SWITCH_MIN_MS) {
    cfgReloadDone();
    lastSwitchMs = millis();
    activePattern->init();
  } else if (cfgReloadTextNeeded()) {
    // The light path: only the text layout, no romLoad and so no black
    // picture; only a card without a reflow escalates to the full init.
    cfgReloadTextDone();
    if (activePattern->reflow) activePattern->reflow();
    else cfgReload();
  }

  // A card chosen by hand stops the slideshow. Before the block below,
  // not after: that one clears the request, so a check behind it would
  // never see one.
  if (slideshowOn && requestedPattern >= 0) slideshowSet(false, false);

  // A card change waits for a running dump too, or the picture going
  // over would change card halfway; the request stays queued.
  if (!dumpActive && !menuOpen && requestedPattern >= 0 && millis() - lastSwitchMs >= SWITCH_MIN_MS) {
    const int target = requestedPattern;
    if (target == patternIndex) {
      requestedPattern = -1;
    } else {
      // The choice goes to flash first, and a write's videoResume()
      // leaves the ring empty, which would wrongly condemn a card
      // judged right after it. So this waits on the ring's own stock,
      // the same threshold the copy itself uses.
      cfgSetStartPattern(target);
      saveCardNow();
      if ((int32_t)(videoProducedCount - videoConsumedCount) < VIDEO_ROM_COPY_KEEP_ROWS) {
        if (switchWaitMs == 0) switchWaitMs = millis();
        if (millis() - switchWaitMs < SWITCH_WAIT_MAX_MS) return;
      }
      switchWaitMs = 0;

      requestedPattern = -1;
      lastSwitchMs = millis();
      selectPattern(target);
    }
  }

  // Paused while a candidate is offered: displaySetCandidate() takes
  // over the whole panel then.
  // Not during a dump (the PC would receive a picture that changes
  // card halfway and stamp it with the first card's CRC) and not under
  // an open card menu (cardMenuBuild() relies on the card standing
  // still); both just pause the show.
  if (slideshowOn && encoderCandidate < 0 && !dumpActive && !menuOpen) slideshowStep();

  // The timing report, non-blocking: only written when the USB buffer
  // can take it straight away, else skipped.
  if (!timingOn) return;
  static uint32_t lastReport = 0;
  uint32_t now = millis();
  if (now - lastReport < 2000) return;
  lastReport = now;

  VideoStats s;
  videoGetStats(&s);
  // 260 was just enough for the old message; two fields have been added since.
  char msg[340];
  int n = snprintf(msg, sizeof(msg),
                   LANG_TIMING_MIN_LU_US_AVG,
                   (unsigned long)s.minUs, (unsigned long)s.avgUs, (unsigned long)s.worstUs,
                   (unsigned long)VIDEO_LINE_BUDGET_US, (unsigned long)s.overBudget,
                   (unsigned long)s.lines, (unsigned long)s.starves, (unsigned long)s.txStalls,
                   (unsigned long)s.chainRestarts,
                   (unsigned long)s.producerStalls,
                   (unsigned long)s.flashLines, (unsigned long)s.loadStalls,
                   (unsigned long)cfgDeviations);
  if (Serial && n > 0 && Serial.availableForWrite() >= n) Serial.write(msg, n);

  // The same three figures over this report window only: the lifetime
  // average above hides a card that is slow right now behind a million
  // fast rows before it (found when the Sony PCM card starved the ring
  // while avg still read 38 us).
  {
    static uint64_t lastSum = 0;
    static uint32_t lastCount = 0;
    const uint64_t sum = videoSumBuildUs;
    const uint32_t cnt = videoSampleCount;
    const uint32_t rows = cnt - lastCount;
    const unsigned long avg = rows ? (unsigned long)((sum - lastSum) / rows) : 0;
    n = snprintf(msg, sizeof(msg), LANG_TIMING_WINDOW, (unsigned long)rows, avg,
                 (unsigned long)timingWindowWorstUs);
    if (Serial && n > 0 && Serial.availableForWrite() >= n) Serial.write(msg, n);
    lastSum = sum;
    lastCount = cnt;
    timingWindowWorstUs = 0;
  }

  // WHAT THE TICKER BAND REALLY COSTS, counted on the board. What fits in
  // a row has been worked out wrongly three times over here, so this is
  // measured: the time in the ticker itself, without the card underneath,
  // and how many glyphs it drew for it. Reading clears it, so this covers
  // the period since the last report.
  uint32_t tr, tsum, tworst, tgl;
  tickerTakeStats(&tr, &tsum, &tworst, &tgl);
  if (tr) {
    n = snprintf(msg, sizeof(msg),
                 LANG_TICKER_S_LU_BAND_ROWS,
                 tickerModeName(), (unsigned long)tr, (unsigned long)(tsum / tr),
                 (unsigned long)tworst, (unsigned long)(tgl / tr));
    if (Serial && n > 0 && Serial.availableForWrite() >= n) Serial.write(msg, n);
  }
}

// What memory this board has, reported rather than looked up: flash
// from PICO_FLASH_SIZE_BYTES and the _FS_start/_FS_end linker symbols;
// SRAM from the SDK's own SRAM_BASE/SRAM_STRIPED_END/SRAM_END (512 kB
// contiguous plus two 4 kB scratch banks, 520 kB together); PSRAM from
// what psram_init() really found at startup (zero means none, not
// "not used").
static void reportMemory() {
  const uint32_t flashTot = PICO_FLASH_SIZE_BYTES;
  const uint32_t reserved = photoReservedBytes();
  const uint32_t eeprom = 4096;
  const uint32_t schets = flashTot - reserved - eeprom;

  Serial.printf(LANG_FLASH_LU_KB_TOTAL,
                (unsigned long)(flashTot / 1024), (unsigned long)(schets / 1024),
                (unsigned long)(reserved / 1024), (unsigned long)(eeprom / 1024));
  Serial.printf(LANG_RESERVED_D_PHOTOGRAPH_SLOTS,
                PHOTO_SLOTS, (unsigned long)((photoSlotSpace() + PHOTO_SECTOR) / 1024),
                (unsigned long)(photoFreeBytes() / 1024));

  const uint32_t sramTot = SRAM_END - SRAM_BASE;
  const uint32_t sramStriped = SRAM_STRIPED_END - SRAM_BASE;
  Serial.printf(LANG_SRAM_LU_KB_ON,
                (unsigned long)(sramTot / 1024), (unsigned long)(sramStriped / 1024),
                (unsigned long)((sramTot - sramStriped) / 1024),
                rp2040.getTotalHeap() / 1024, rp2040.getFreeHeap() / 1024);

  size_t ps = rp2040.getPSRAMSize();
  if (ps) {
    Serial.printf(LANG_PSRAM_LU_KB_FOUND,
                  (unsigned long)(ps / 1024));
  } else {
    Serial.printf(LANG_PSRAM_NONE_FOUND);
  }
}


void setup() {
  // The system clock first, before anything depends on it: under
  // PlatformIO the board already boots at this speed, under the Arduino
  // IDE it boots at whatever the CPU Speed menu says, none of which is
  // a multiple of 54 MHz. See CFG_SYSCLK_HZ in config.h. Core1 is
  // already running but parked in flashSpeedWaitReady(), and switching
  // the PLL is glitchless.
  if (clock_get_hz(clk_sys) != CFG_SYSCLK_HZ) {
    set_sys_clock_khz(CFG_SYSCLK_HZ / 1000, true);
  }

  // Then, before the encoder comes out of reset: otherwise it
  // sees an undefined level for a moment at startup.
  pinMode(PIN_VSYNC, OUTPUT);
  pinMode(PIN_SFL, OUTPUT);
  pinMode(PIN_ACTIVITY_LED, OUTPUT);
  digitalWrite(PIN_VSYNC, HIGH);
  digitalWrite(PIN_SFL, LOW);
  digitalWrite(PIN_ACTIVITY_LED, LOW);

  // PIN_HSYNC (GP13) is not held high by hand any more: pcm1808Begin()
  // hands it to pio1 as the PCM1808's SCKI output instead. Safe for the
  // same reason the old static level was safe, see the wiring comment
  // at the top of this file.
  pcm1808Begin();

  // The front panel (display + knob) is a separate PCB on a flatcable;
  // that board ties this pin low, so it reads low when fitted and high
  // (nothing pulling it, only the pull-up here) when it is not. Read
  // once, before anything on that flatcable is touched.
  pinMode(PIN_FRONT_PRESENT, INPUT_PULLUP);
  const bool frontPresent = digitalRead(PIN_FRONT_PRESENT) == LOW;

  // The panel before the serial port, so there is something to look at
  // during the up-to-three-second serial wait below. The splash appears
  // about half a second after power (the ST7789's own reset and
  // sleep-out delays); everything it needs is compile time
  // configuration, nothing loaded from flash first.
  if (frontPresent) displayBegin();

  // A factory reset on a held button at power-on. Read as early as
  // possible, right after the panel is up, so the countdown starts the
  // moment a held button could plausibly be noticed, not after however
  // long the rest of the bring-up takes.
  //
  // Encoder pins but not encoderBegin(): that also arms the quadrature
  // interrupts this only needs a pull-up for; calling it again later is
  // harmless.
  //
  // The countdown itself never touches flash, only display calls and
  // digitalRead(), so it is safe this early. bootReset only records the
  // choice; the actual flash writes it drives run later in setup(), at
  // points already proven safe for one.
  bool bootReset = false;
#if CFG_ENCODER_ON
  // Only after a cold power-on: watchdog_caused_reboot() is also true
  // for software reboots (key g, the PC tool's update path), and a
  // watchdog reboot with the knob coincidentally still held (a 2 s
  // menu hold during a crash) must not count down to a wipe of the
  // settings and every uploaded photograph.
  if (frontPresent && !watchdog_caused_reboot()) {
    pinMode(PIN_ENC_PUSH, INPUT_PULLUP);
    // Confirmed held for 20 ms before trusting it, the same duration
    // PUSH_STABLE needs for this pin later in the boot (encoder.cpp),
    // rejecting a moment of settling or noise reading as pressed.
    bool held = true;
    uint32_t downSince = millis();
    while (millis() - downSince < 20) {
      if (digitalRead(PIN_ENC_PUSH)) {
        held = false;
        break;
      }
      delay(1);
    }
    if (held) {
      for (int left = 5; left >= 1; left--) {
        displayBootResetCountdown(left);
        uint32_t tickStart = millis();
        bool released = false;
        while (millis() - tickStart < 1000) {
          if (digitalRead(PIN_ENC_PUSH)) {
            released = true;
            break;
          }
          delay(10);
        }
        if (released) {
          // Without this the countdown's last line stayed on the panel
          // for the rest of the bring-up; nothing else redraws the
          // title once this has overwritten it.
          displayBootSplashRestore();
          break;
        }
        if (left == 1) bootReset = true;  // held through the whole countdown
      }
      if (bootReset) displayBootResetCountdown(0);
    }
  }
#endif

  Serial.begin(115200);
  // Waits at most 3 s for a serial monitor, not forever, or the
  // bring-up never starts without a PC attached. The bar creeps
  // proportionally through this wait so it does not read as a hang;
  // with a monitor already attached it just jumps on early.
  uint32_t waitStart = millis();
  while (!Serial && (millis() - waitStart) < 3000) {
    displayBootProgress(3 + (int)((millis() - waitStart) * 22 / 3000));
    delay(10);
  }
  delay(200);
  displayBootProgress(25);

  // The serial port comes before the QMI, deliberately: flashSpeedApply()
  // changes the clock speed of the flash all the code is read from, and
  // if that goes wrong the board stands still with a USB port that says
  // nothing more, so reporting first means always seeing how far it got.
  // Core1 waits in flashSpeedWaitReady() meanwhile.
  Serial.println();
  Serial.println(LANG_PAL_SIGN_ADVANCED_CVBS);
  Serial.printf(LANG_BOARD_S, BOARD_LABEL);
  Serial.printf(LANG_PINS_P0_P7_ON,
                PIN_DATA_BASE, (unsigned)(PIN_DATA_BASE + 7), PIN_CLKOUT, PIN_HSYNC,
                PIN_VSYNC, PIN_SFL, PIN_RESET, PIN_SDA, PIN_SCL);
  reportMemory();
  Serial.printf(LANG_FLASH_DIVISOR_U_AS, flashSpeedDiv());
  Serial.flush();
  flashSpeedApply();  // core1 waits on this
  displayBootProgress(30);

  cfgInit();
  displayBootProgress(35);

  // The RAM half of the boot reset from above, right after cfgInit()
  // and before anything below reads a setting. No flash write here
  // (cfgFactoryReset() only touches the RAM copy, see settings.h), so
  // no videoSuspend() needed; the flash write that makes it permanent
  // is further down, alongside the photograph slot erase.
  if (bootReset) {
    cfgFactoryReset();
    Serial.println(LANG_RESET_FACTORY_VALUES_LOADED_SAVING);
  }

  // Which card carries what, told to the settings once: comparing
  // pointers cannot be broken by renaming a card. Used to leave a
  // shared text out of a checksum where it cannot appear, and by the
  // dump to say which card has a switch of its own.
  for (int i = 0; i < PATTERN_COUNT; i++) {
    uint8_t t = 0;
    if (PATTERNS[i] == &PATTERN_CONTEST) t |= CARD_TRAIT_CONTEST;
    // The twelve that draw the identification: also observed through
    // cfgTextIdShown() while a card lays itself out, which wins where
    // there is one; written out here too since a never-shown card has
    // nothing to observe. cfgSetActiveCard() complains if this list is wrong.
    if (PATTERNS[i] == &PATTERN_PM5644G00 || PATTERNS[i] == &PATTERN_PM5644G924 ||
        PATTERNS[i] == &PATTERN_FUBK4X3 || PATTERNS[i] == &PATTERN_FUBKNOCIRCLE ||
        PATTERNS[i] == &PATTERN_FUBK16X9 || PATTERNS[i] == &PATTERN_TESTCARDF ||
        PATTERNS[i] == &PATTERN_TESTCARDW || PATTERNS[i] == &PATTERN_TVE ||
        PATTERNS[i] == &PATTERN_TESTCARDG || PATTERNS[i] == &PATTERN_EBUBW ||
        PATTERNS[i] == &PATTERN_BARSRED || PATTERNS[i] == &PATTERN_MIXEDBAR ||
        PATTERNS[i] == &PATTERN_PICDREAM || PATTERNS[i] == &PATTERN_PCMAUDIO)
      t |= CARD_TRAIT_ID;
    // THE EIGHT THAT DRAW THE SECOND TEXT LINE, found the same way:
    // grep -l cfgTextSubShown src/testcards/*.cpp
    if (PATTERNS[i] == &PATTERN_PM5644G00 || PATTERNS[i] == &PATTERN_PM5644G924 ||
        PATTERNS[i] == &PATTERN_FUBK4X3 || PATTERNS[i] == &PATTERN_FUBK16X9 ||
        PATTERNS[i] == &PATTERN_TESTCARDG || PATTERNS[i] == &PATTERN_TVE ||
        PATTERNS[i] == &PATTERN_EBUBW || PATTERNS[i] == &PATTERN_BARSRED ||
        PATTERNS[i] == &PATTERN_PCMAUDIO)
      t |= CARD_TRAIT_SUB;
    // THE THREE THAT DRAW THE INSERT BOXES, found the same way:
    // grep -l insertPrepare src/testcards/*.cpp
    if (PATTERNS[i] == &PATTERN_PM5644G00 || PATTERNS[i] == &PATTERN_PM5644G924 ||
        PATTERNS[i] == &PATTERN_TVE)
      t |= CARD_TRAIT_BOX;
    if (PATTERNS[i] == &PATTERN_PM5644G924) t |= CARD_TRAIT_G924;
    if (PATTERNS[i] == &PATTERN_MIXEDBAR || PATTERNS[i] == &PATTERN_PICDREAM)
      t |= CARD_TRAIT_FIXEDFONT;
    // CARD_TRAIT_CARDG is handed out to nobody: the G was rebuilt from
    // the EPROM tables, the AP1/AP2 block was bbc-testcards' own. The
    // stored tcgap2 setting sleeps unused; see testcardg.cpp.
    // The nine with a box for the moving line, found the same way:
    // grep -l movingLineBox src/testcards/*.cpp
    if (PATTERNS[i] == &PATTERN_PM5644G00 || PATTERNS[i] == &PATTERN_PM5644G924 ||
        PATTERNS[i] == &PATTERN_FUBK4X3 || PATTERNS[i] == &PATTERN_FUBKNOCIRCLE ||
        PATTERNS[i] == &PATTERN_FUBK16X9 || PATTERNS[i] == &PATTERN_TESTCARDF ||
        PATTERNS[i] == &PATTERN_TESTCARDW || PATTERNS[i] == &PATTERN_TVE ||
        PATTERNS[i] == &PATTERN_TESTCARDG)
      t |= CARD_TRAIT_LINE;
    cfgSetCardTraits(i, t);
  }
  // The band is laid out further down, once the card it belongs to has
  // been chosen: its row, its height and its face are kept per card.

  Serial.println(LANG_MAKE_SURE_JP1_IS);
  {
    unsigned div = flashSpeedDiv();
    unsigned kbs = flashSpeedMeasureKBs();
    Serial.printf(LANG_FLASH_DIVISOR_U_QSPI_2, div,
                  (unsigned long)(CFG_SYSCLK_HZ / div / 1000000), (unsigned long)(CFG_SYSCLK_HZ / div / 100000) % 10,
                  kbs / 1000, (kbs / 100) % 10);
  }

  // Reports the clock chain: sysclk / divisor / 2 must be exactly
  // 27.000000 MHz, or the clock choice in platformio.ini is wrong.
  {
    const uint32_t sysclk = clock_get_hz(clk_sys);
    const int div = sysclk / 54000000;
    uint32_t clkin = sysclk / (div * 2);
    Serial.printf(LANG_CLOCK_SYSCLK_LU_03LU,
                  (unsigned long)(sysclk / 1000000), (unsigned long)((sysclk / 1000) % 1000), div,
                  (unsigned long)(clkin / 1000000), (unsigned long)(clkin % 1000000));
  }

  displayBootProgress(40);
  Serial.println(LANG_0_WAITING_FOR_CORE1_TO);
  while (!videoClockReady()) delay(1);
  Serial.println(LANG_OK);
  displayBootProgress(45);

  if (!advBringUp()) {
    Serial.println(LANG_ERROR_THE_ENCODER_DOES);
    return;
  }

  // The clock, after advBringUp() because the RTC probe needs the I2C
  // bus: from the RV-3028's supercap-backed time when the board has
  // one and it was ever set, else the start values from config.h.
  if (clockBegin()) {
    ClockTime ct;
    clockGet(&ct);
    uint8_t st = 0, bk = 0;
    rv3028Peek(&st, &bk);
    Serial.printf(LANG_CLOCK_FROM_RTC_04D, ct.year, ct.month, ct.day, ct.hour, ct.min, ct.sec,
                  st, bk);
  } else {
    // Three answers, not two: a chip that is there but lost its time
    // (PORF) points at the supercap or the switchover, not the bus.
    uint8_t st = 0, bk = 0;
    if (rv3028Peek(&st, &bk)) Serial.printf(LANG_CLOCK_RTC_TIME_LOST, st, bk);
    else Serial.println(LANG_CLOCK_NO_RTC_STARTS);
  }

  // The aspect ratio is applied with the card, further down. CFG_ASPECT
  // is only the value a card starts at when nothing was ever saved.

  // Checked once, here and not later: the producer loop is not running
  // yet, so these CRCs over 810 kB per slot hold nothing up. Done while
  // loading a test card, this made the DMA chain fall over and the
  // picture stayed black.
  {
    uint32_t t0 = millis();
    // The slowest stretch after the serial wait, so the bar moves per
    // slot: sixteen CRCs over up to 810 kB each.
    for (int i = 0; i < PHOTO_SLOTS; i++) {
      photoCheck(i);
      displayBootProgress(50 + (i + 1) * 30 / PHOTO_SLOTS);
    }
    // Same reasoning, far smaller (at most 136 kB, not 810 kB): the two
    // BBC Test Card F/W photo replacements, see portrait.h.
    for (int i = 0; i < PORTRAIT_COUNT; i++) portraitCheck(i);
    Serial.printf(LANG_PHOTOGRAPH_SLOTS_CHECKED_IN, (unsigned long)(millis() - t0));
    printPhotoSlots();
  }

  // The card the board last stood on, out of the saved settings. After
  // the photo check above, since an empty slot shows flat grey and
  // whether a slot is filled is only known once those CRCs have run.
  // Falls back to the first card if the saved index is out of range
  // (a shorter list than last time) or its slot was emptied since.
  {
    int want = cfgStartPattern();
    if (want < 0 || want >= PATTERN_COUNT) want = 0;
    const int slot = customSlotOf(PATTERNS[want]);
    if (slot >= 0 && !photoValid(slot)) want = 0;
    if (!cardIsAvailable(want)) want = 0;   // a PCM card whose ADC is gone
    patternIndex = want;
    activePattern = PATTERNS[want];
    // Before any card lays itself out: its identification setting is kept
    // per card and the init has to see the right one.
    cfgSetActiveCard(want);
    tickerAllowTransparent(customSlotOf(PATTERNS[want]) < 0);
    // THE CARD BRINGS ITS OWN, here as well as in selectPattern(). This
    // path does not go through selectPattern, so without these three the
    // board came up on the remembered card with card 1's band and with
    // the aspect ratio out of config.h.
    aspectSet(cfgCardAspect(-1));
    insertSetMode(cfgCardInsert(-1));
    textInkRefresh();
    textSetFont(cfgCardTextFont(-1));
    tickerBegin();
    Serial.printf(LANG_STARTING_ON_CARD_D, want + 1, activePattern->name);
  }
  displayBootProgress(85);

  // THE SLIDESHOW COMES BACK BY ITSELF. Whether it was running is in
  // flash beside the card (cfgSlideshowOn), so a power cut in the
  // middle of a show is a pause and not a stop. slideshowSet() checks
  // again whether there is anything to show -- the photographs can have
  // been emptied since -- and takes the flag down when there is not.
  if (cfgSlideshowOn()) slideshowSet(true);

  // The encoder settings follow the card, so they are set from the one
  // that has just been chosen. selectPattern() does this itself from
  // here on.
  advSetMonochrome(activePattern->mono);
  advSetColourBars(activePattern->encoderMakesIt);

  // VBI open. Without this bit the whole vertical blanking is blanked
  // away and no test signal can ever sit in the blanking.
  advSetVbiOpen(true);

  // Teletext: builds the pulse table and rasterises every packet once.
  // Has to happen HERE, before loop() in Firmware.ino starts: it costs a
  // few milliseconds and no picture row is built meanwhile.
  {
    uint32_t t0 = micros();
    teletextInit();
    Serial.printf(LANG_TELETEXT_WORKED_OUT_IN, (unsigned long)(micros() - t0));
    printTeletext();
  }
  displayBootProgress(92);

  // Fixes the PAL eight field sequence on our own field 1, or the
  // encoder's field counter runs free from power-on and the subcarrier
  // phase lands differently every time. Only requested here since the
  // producer loop isn't running yet; the frame loop finishes it once
  // the streamer reaches the line boundary, see sflFinish().
  sflTimingReset();

  // The flash half of the boot reset from the top of setup(), deferred
  // to here: cfgJournalMaintenance() right below already does its own
  // flash erase every boot, proof videoSuspend() succeeds at this
  // point, unlike at the too-early countdown earlier in setup().
  if (bootReset) {
    // Erases every filled photograph slot, the same operation
    // photoEraseSlot() does for one slot, fourteen times over.
    if (videoSuspend()) {
      int erased = 0;
      for (int i = 0; i < PHOTO_SLOTS; i++) {
        if (photoHeaderOk(i)) {
          photoBeginWrite(i);
          erased++;
        }
      }
      // Small next to the photo slots (at most 136 kB), so erased
      // unconditionally rather than checked first; see portrait.h.
      for (int i = 0; i < PORTRAIT_COUNT; i++) portraitBeginWrite(i);
      videoResume();
      Serial.printf(LANG_RESET_D_PHOTOGRAPH_SLOT_S, erased);
    } else {
      Serial.println(LANG_RESET_CORE1_DID_NOT_PARK);
    }
    // The CRC cache from the check further up is now stale for whatever
    // just got erased; still before the producer starts, so rerunning
    // photoCheck() here is the same safe moment as the first time.
    for (int i = 0; i < PHOTO_SLOTS; i++) photoCheck(i);
    for (int i = 0; i < PORTRAIT_COUNT; i++) portraitCheck(i);
    printPhotoSlots();

    if (cfgSave()) {
      Serial.println(LANG_RESET_FACTORY_SETTINGS_SAVED_THE);
    } else {
      Serial.println(LANG_RESET_FACTORY_SETTINGS_NOT_SAVED);
    }
  }

  // Journal maintenance while the screen is still dark: the producer
  // has not started, the streamer sends black, so erasing spent
  // settings sectors here is invisible, buying the session its
  // flash-free saves.
  cfgJournalMaintenance();
  displayBootProgress(100);

  // The splash has been on the panel since the top of setup(); from
  // here the working layout takes over. The full bar stands for a
  // moment first, so the eye sees it arrive instead of vanish at 92.
  delay(120);
  displaySplashDone();
  if (frontPresent) encoderBegin();
  if (displayOn()) {
    char buf[160];  // displayReport is a long line
    displayReport(buf, sizeof(buf));
    Serial.printf(LANG_DISPLAY_DX_D_ST7789P3_S, DISP_W, DISP_H, buf);
    Serial.println(LANG_L_DRAWS_THE_ALIGNMENT);
  }

  Serial.println(LANG_BRING_UP_FINISHED);
  Serial.println(LANG_TEST_CARDS);
  printPatterns();
  printAspect();
  printLumaFilter();
  Serial.printf(LANG_TIMING_REPORT_IS_S,
                timingOn ? LANG_ON : LANG_OFF);

  // Sets the pattern up and hooks on to the streamer. The rendering
  // itself is loop(), one row per turn.
  videoProducerBegin();

  // The watchdog, armed last: core1 has been streaming since well
  // before this point, so videoFeedWatchdog() has something real to
  // check from its first call. 8000 ms sits comfortably above every
  // bounded wait in this codebase (the 500 ms producer stall, the
  // 400 ms max flash erase, the 5000 ms UPLOAD_TIMEOUT_MS) and well
  // under the RP2350's own 16777 ms ceiling.
  rp2040.wdt_begin(8000);
}

// Core0: the producer. One picture row per turn, rendered into the ring
// buffer core1 empties, with the card change and status display pushed
// along in the time left over.
//
// __loop(), what runs between two turns, is cheap enough to leave
// alone: with the Pico SDK USB stack (the sketch.yaml default) it is
// just arduino::serialEventRun() finding a null serialEvent() and
// returning, about ten instructions, 0.1 us of the 32 us a row has to
// spare. Picking Adafruit TinyUSB adds yield() polling the USB stack,
// and this no longer holds.
void loop() {
  static uint32_t line = 1;
  // Hooks back onto the streamer after a videoSuspend()/videoResume():
  // it carried on with blank rows, so its line counter sits somewhere
  // mid-picture, given by "ring slot k belongs to line (k % 625) + 1".
  // Not line 1: the line and field structure was never interrupted, so
  // jumping back to line 1 would give a picture that is too short.
  if (videoRestartRequest) {
    videoRestartRequest = false;
    line = (videoProducedCount % VIDEO_LINES_PER_FRAME) + 1;
  }
  // VIDEO_RING_SIZE - 3: the chained DMA always has two rows under way
  // (being sent, and already loaded into the other channel), which the
  // producer must keep away from.
  //
  // Waits for core1, but not forever: if the streamer never comes back
  // core0 would stand here for good and the whole board would be dead,
  // which is exactly what happened after a failed upload once. After
  // half a second it carries on anyway, disturbing the picture but
  // keeping the board usable; the counter below makes it visible.
  {
    uint32_t t0 = time_us_32();
    // Signed compare: just after videoResume() the producer can be
    // briefly behind the streamer, which would flip the unsigned
    // difference to nearly 2^32 and trigger an unneeded half-second wait.
    while ((int32_t)(videoProducedCount - videoConsumedCount) >= (int32_t)(VIDEO_RING_SIZE - 3)) {
      if (time_us_32() - t0 > 500000) {
        videoStallCount++;
        break;
      }
      tight_loop_contents();
    }
  }

  // Two answers, since the display's two halves are not equally
  // expensive: sampling the miniature (26 us) fits outside a card
  // change with room to spare, but the 152 us transfer pulls the stock
  // down and waits for room. Gating both on the stock was too strict:
  // a single unlucky missed band could leave a card like TVE black for
  // good, since the miniature only sends once all 76 bands are in.
  //
  // The encoder is read here, once per row (every 64 us): a hand
  // turning twenty detents a second is eighty edges against 15625
  // samples, three orders of margin, and the 25 Hz frame hook would
  // drop steps.
  encoderPoll();

  const bool noCardChange = !romCopyPending();
  // The ticker gets the same treatment as the display, and for the same
  // reason: the copy only runs while the ring buffer is above its
  // threshold, and the band is what pulls it down.
  tickerPause(!noCardChange);
  videoDisplaySample = noCardChange;
  videoDisplayMayWork = noCardChange &&
                   (int32_t)(videoProducedCount - videoConsumedCount) > VIDEO_DISPLAY_KEEP_ROWS;
  uint32_t t0 = time_us_32();
  videoBuildLine(videoRing[videoProducedCount % VIDEO_RING_SIZE], line);
  uint32_t dt = time_us_32() - t0;
  if (dt > videoWorstBuildUs) videoWorstBuildUs = dt;
  if (dt < videoMinBuildUs) videoMinBuildUs = dt;
  if (dt > timingWindowWorstUs) timingWindowWorstUs = dt;
  videoSumBuildUs += dt;
  videoSampleCount++;
  if (dt > VIDEO_LINE_BUDGET_US) videoOverBudgetCount++;

  // Pushes a card change along: romLoad() hands its table copy in as a
  // job rather than all at once, only while the ring buffer holds more
  // than VIDEO_ROM_COPY_KEEP_ROWS rows, which is what makes it safe: it
  // only uses time the producer would otherwise wait on core1, and
  // gives that time back the moment the stock runs down.
  videoProducedCount++;
  if (romCopyPending()) {
    int32_t stock = (int32_t)(videoProducedCount - videoConsumedCount);
    if (stock < VIDEO_ROM_COPY_PANIC_ROWS) {
      // Decoding from flash costs more per row than from RAM; on the
      // heaviest card, more than a row's 64 us (measured 64.4 us for a
      // PM5644 16:9 row's decode), so that card never catches up and
      // the copy never gets its turn.
      //
      // So when the stock really runs down, finish the copy in one go:
      // a card that fits gets a clean switch, one that does not falls
      // back here and pays the KEEP-to-PANIC stock, about 1 ms.
      romCopyPanic();
    } else {
      while ((int32_t)(videoProducedCount - videoConsumedCount) > VIDEO_ROM_COPY_KEEP_ROWS) {
        if (!romCopyStep(VIDEO_ROM_COPY_CHUNK)) break;
      }
    }
  }

  // The transfer, under the same rule as the sampling. See
  // videoDisplayMayWork above. While a card dump runs, the display
  // only takes a slot with four rows more in stock: it comes first in
  // this loop and a 152 us chunk costs 2.4 rows of stock, so on a card
  // whose rows take 55 us (Ham PCM LQ) it pulled the stock back to the
  // threshold every time and the dump below never got a turn (the PC
  // tool sat on "fetching" with not one row received). The band from
  // KEEP to KEEP + 4 is the dump's.
  const bool displayMay = videoDisplayMayWork &&
      (!dumpActive || (int32_t)(videoProducedCount - videoConsumedCount) > VIDEO_DISPLAY_KEEP_ROWS + 4);
  if (displayMay && displayBusy()) displayStep(VIDEO_DISPLAY_CHUNK);

  // The card dump, under the same rule again: one bounded piece per
  // spare slot, and only while the stock can carry it. See
  // cardDumpStep().
  if (dumpActive && noCardChange &&
      (int32_t)(videoProducedCount - videoConsumedCount) > VIDEO_DISPLAY_KEEP_ROWS)
    cardDumpStep();

  line++;
  if (line > VIDEO_LINES_PER_FRAME) {
    line = 1;
    videoFrameHook();
  }
}

// --- Core1 -----------------------------------------------------------
void setup1() {
  flashSpeedWaitReady();
  videoStartPio();
  videoStreamBegin();
}

// Core1: the streamer. One row per turn, never waits.
//
// __not_in_flash_func is not a luxury here: while core0 writes to
// flash, core1 may not touch the XIP space, and this way it carries on.
//
// Between two turns core1 is in flash, in main1()'s while loop, which
// is allowed: core0 does not start erasing until videoParked is true,
// set only at the bottom of this function from inside the wait that
// follows it, so core1 is always inside this function, in RAM, for the
// whole of a write.
//
// Check after every change with arm-none-eabi-objdump -d on the elf:
// loop1, videoLoadLine, lineKind, videoKickChain, videoSflStep and
// videoCountTxStall must all sit at an address in 0x2000_0000.., and no
// bl out of them may go to 0x1000_0000..
void __not_in_flash_func(loop1)() {
  static int arm = 0;      // 0 = reload channel A, 1 = channel B
  int ch = arm ? videoDmaChanB : videoDmaChanA;
  videoLoadLine(ch, false);
  arm ^= 1;
  videoKickChain(ch);
  videoSflStep();
  videoCountTxStall();

  // Core0 wants to write to flash: from here to the end of this loop no
  // instruction runs out of XIP space and interrupts are off; the chain
  // carries on with blank rows.
  if (videoParkRequest) {
    // save_and_disable_interrupts() from the pico-sdk, not
    // noInterrupts() from the Arduino layer: that one lives in flash
    // (arm-none-eabi-nm: 1000ce10 T interrupts, 1000ce44 T
    // noInterrupts), called through a veneer, and would give exactly
    // the bus fault this is avoiding. The sdk variant produces bare
    // instructions; the disassembly shows mrs/cpsid/msr and no bl.
    uint32_t st = save_and_disable_interrupts();
    videoParked = true;
    while (videoParkRequest) {
      int c = arm ? videoDmaChanB : videoDmaChanA;
      videoLoadLine(c, true);
      arm ^= 1;
      videoKickChain(c);
      videoSflStep();
      videoCountTxStall();
    }
    videoParked = false;
    restore_interrupts(st);
  }
}

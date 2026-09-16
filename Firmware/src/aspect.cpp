#include "aspect.h"

#include "adv7391.h"
#include "gfx.h"
#include "pattern.h"
#include "ticker.h"

// WSS word: 14 bits, group 1 (bits 3..0) is the aspect ratio, groups 2-4
// are zero (camera mode, no subtitles/reserved). Codes from AN9716 table 2
// / ADV7391 datasheet table 54 (page 69).
struct AspectDef {
  const char *name;
  uint16_t wss;
  int activeLines;
  bool top;   // picture against the top edge instead of centred
};

static const AspectDef ASPECTS[ASPECT_COUNT] = {
    {"off (no WSS)", 0x0000, 576, false},
    {"4:3 full frame", 0x0008, 576, false},
    {"14:9 letterbox", 0x0001, 504, false},
    {"14:9 letterbox at the top", 0x0002, 504, true},
    {"14:9 full frame", 0x000E, 576, false},
    {"16:9 letterbox", 0x000B, 430, false},
    {"16:9 letterbox at the top", 0x0004, 430, true},
    {"16:9 anamorphic", 0x0007, 576, false},
    {"wider than 16:9, letterbox", 0x000D, 326, false},   // not specified by the standard
};

static int current = ASPECT_OFF;
static int bar = 0;
static int active = SCREEN_H;

int aspectGet() { return current; }

const char *aspectName(int mode) {
  if (mode < 0 || mode >= ASPECT_COUNT) return "?";
  return ASPECTS[mode].name;
}

void aspectSet(int mode) {
  if (mode < 0 || mode >= ASPECT_COUNT) return;
  current = mode;
  active = ASPECTS[mode].activeLines;
  bar = ASPECTS[mode].top ? 0 : (((SCREEN_H - active) / 2) & ~1);   // even, or the bar shimmers
  advSetWss(mode != ASPECT_OFF, ASPECTS[mode].wss);
}

static void blackRow(uint8_t *base) {
  uint32_t *w = (uint32_t *)base;
  for (int i = 0; i < SCREEN_W / 2; i++) *w++ = 0x10801080u;   // Cb,Y,Cr,Y: neutral chroma, black luma
}

static void renderCardRow(uint8_t *base, int y) {
  if (y == 0 && current != ASPECT_OFF) {   // line 23, where the WSS burst goes
    blackRow(base);
    return;
  }
  if (active >= SCREEN_H) {
    activePattern->renderRow(base, y);
    return;
  }
  if (y < bar || y >= bar + active) {
    blackRow(base);
    return;
  }
  activePattern->renderRow(base, (y - bar) * SCREEN_H / active);   // squeezed by skipping rows
}

void patternRenderRow(uint8_t *base, int y) {
  // A raw-signal card (pattern.h) gets neither bars nor ticker.
  if (activePattern->rawSignal) {
    activePattern->renderRow(base, y);
    return;
  }
  // Line 23 carries the WSS burst; the blackout must also beat a
  // solid band dragged to the very top (bandY 0 is a legal position).
  if (y == 0 && current != ASPECT_OFF) {
    blackRow(base);
    return;
  }
  if (tickerSolidRow(y)) {
    tickerRenderRow(base, y);
    return;
  }
  renderCardRow(base, y);
  tickerOverlayRow(base, y);
}

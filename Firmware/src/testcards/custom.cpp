// The uploadable photographs, one test card per flash slot. The slot
// number is baked into each of the fifteen via CUSTOM_CARD(n) below,
// since a Pattern's renderRow(base, y) has nowhere to put one; init()
// records the slot and the shared renderRow() reads it back.
//
// The row is read with memcpy and that must not become a DMA: a DMA
// channel on the QSPI holds the bus long enough to starve the video
// channels, and PSRAM / the no-allocate flash window are both slower
// than the cached read here.
//
// An empty slot draws neutral grey rather than raw erased flash (0xFF,
// which is a bright colour in 4:2:2, not grey).
#include <string.h>

#include "../photoflash.h"
#include "../gfx.h"
#include "../pattern.h"

static const uint32_t GREY_WORD = 0x80808080u;   // Y=Cb=Cr=0x80: neutral mid grey

// Which slot the active card reads. photoValid() is cheap but the CRC
// under it runs over 810 kB, so this is read once at init, not per row.
static int activeSlot = 0;
static bool activeValid = false;

static void customInit(int slot) {
  activeSlot = slot;
  activeValid = photoValid(slot);
}

static void customRenderRow(uint8_t *base, int y) {
  if (activeValid) {
    memcpy(base, photoData(activeSlot) + (unsigned)y * (SCREEN_W * 2), SCREEN_W * 2);
    return;
  }
  uint32_t *w = (uint32_t *)base;
  for (int i = 0; i < SCREEN_W / 2; i++) *w++ = GREY_WORD;
}

// One init function and one Pattern per slot, built by the preprocessor
// so the menu number and the upload-prompt number can never drift apart.
#define CUSTOM_CARD(n)                                                          \
  static void custom##n##Init() { customInit((n) - 1); }                        \
  const Pattern PATTERN_CUSTOM##n = {"Custom " #n " (uploadable, key u)",       \
                                     custom##n##Init, customRenderRow, nullptr, \
                                     false, false}

CUSTOM_CARD(1);
CUSTOM_CARD(2);
CUSTOM_CARD(3);
CUSTOM_CARD(4);
CUSTOM_CARD(5);
CUSTOM_CARD(6);
CUSTOM_CARD(7);
CUSTOM_CARD(8);
CUSTOM_CARD(9);
CUSTOM_CARD(10);
CUSTOM_CARD(11);
CUSTOM_CARD(12);
CUSTOM_CARD(13);
CUSTOM_CARD(14);

// Which slot a card belongs to, or -1 if it isn't one of these; the menu
// and the status display both ask here rather than duplicating the list.
static const Pattern *const CUSTOM_CARDS[PHOTO_SLOTS] = {
    &PATTERN_CUSTOM1,  &PATTERN_CUSTOM2,  &PATTERN_CUSTOM3,  &PATTERN_CUSTOM4,
    &PATTERN_CUSTOM5,  &PATTERN_CUSTOM6,  &PATTERN_CUSTOM7,  &PATTERN_CUSTOM8,
    &PATTERN_CUSTOM9,  &PATTERN_CUSTOM10, &PATTERN_CUSTOM11, &PATTERN_CUSTOM12,
    &PATTERN_CUSTOM13, &PATTERN_CUSTOM14};

int customSlotOf(const Pattern *p) {
  for (int i = 0; i < PHOTO_SLOTS; i++) {
    if (CUSTOM_CARDS[i] == p) return i;
  }
  return -1;
}

const char *customFileName(const Pattern *p) {
  const int slot = customSlotOf(p);
  if (slot < 0 || !photoValid(slot)) return nullptr;
  const PhotoHeader *h = photoHeader(slot);
  return (h && h->name[0]) ? h->name : nullptr;
}

static_assert(PHOTO_SLOTS <= 14, "add CUSTOM_CARD entries to match PHOTO_SLOTS");

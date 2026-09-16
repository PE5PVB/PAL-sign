#include "photoflash.h"

#include <Arduino.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "video.h"

// Supplied by the linker (memmap_default.ld). Declared as arrays, not a
// single uint8_t, since a linker symbol has no size of its own.
extern "C" uint8_t _FS_start[];
extern "C" uint8_t _FS_end[];

static const uint32_t DATA_OFFSET = PHOTO_SECTOR;  // sector 0 is de kop
static const uint32_t DATA_SECTORS = (PHOTO_BYTES + PHOTO_SECTOR - 1) / PHOTO_SECTOR;  // 203
static const uint32_t SLOT_BYTES = (1 + DATA_SECTORS) * PHOTO_SECTOR;               // 835584

static bool validSlot(int slot) { return slot >= 0 && slot < PHOTO_SLOTS; }

static const uint8_t *slotBase(int slot) {
  return _FS_start + (uint32_t)slot * SLOT_BYTES;
}

static uint32_t slotFlashOffset(int slot) {
  return (uint32_t)((uintptr_t)slotBase(slot) - XIP_BASE);
}

uint32_t photoSlotSpace() {
  uint32_t total = (uint32_t)(_FS_end - _FS_start);
  if (total < (uint32_t)PHOTO_SLOTS * SLOT_BYTES) return 0;
  return SLOT_BYTES - DATA_OFFSET;
}

const PhotoHeader *photoHeader(int slot) { return (const PhotoHeader *)slotBase(slot); }
const uint8_t *photoData(int slot) { return slotBase(slot) + DATA_OFFSET; }

uint32_t photoFreeOffset() {
  return (uint32_t)((uintptr_t)(_FS_start + (uint32_t)PHOTO_SLOTS * SLOT_BYTES) - XIP_BASE);
}

uint32_t photoReservedBytes() { return (uint32_t)(_FS_end - _FS_start); }

uint32_t photoFreeBytes() {
  uint32_t total = (uint32_t)(_FS_end - _FS_start);
  uint32_t used = (uint32_t)PHOTO_SLOTS * SLOT_BYTES;
  return total > used ? total - used : 0;
}

uint32_t photoCrc32(const uint8_t *data, uint32_t len, uint32_t crc) {
  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return crc;
}

uint32_t photoCrcFromFlash(int slot) {
  return photoCrc32(photoData(slot), PHOTO_BYTES, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
}

// Expensive (829440 bytes), so run once per slot before streaming, not
// during it; only the cheap header check runs after that.
static bool crcChecked[PHOTO_SLOTS];
static bool crcOk[PHOTO_SLOTS];

bool photoHeaderOk(int slot) {
  if (!validSlot(slot)) return false;
  const PhotoHeader *k = photoHeader(slot);
  if (k->magic != PHOTO_MAGIC) return false;
  if (k->bytes != PHOTO_BYTES) return false;
  if (k->width != 720 || k->height != 576) return false;
  return true;
}

bool photoValid(int slot) {
  return validSlot(slot) && photoHeaderOk(slot) && crcChecked[slot] && crcOk[slot];
}

bool photoCheck(int slot) {
  if (!validSlot(slot)) return false;
  crcChecked[slot] = true;
  crcOk[slot] = photoHeaderOk(slot) && (photoCrcFromFlash(slot) == photoHeader(slot)->crc);
  return crcOk[slot];
}

// The caller must already have parked core1 with videoSuspend(); only
// the interrupts need turning off here, for core0's own flash reads.
static void eraseSector(uint32_t offset) {
  noInterrupts();
  flash_range_erase(offset, PHOTO_SECTOR);
  interrupts();
}

static void programSector(uint32_t offset, const uint8_t *data, uint32_t len) {
  noInterrupts();
  flash_range_program(offset, data, len);
  interrupts();
}

// All sectors erase up front: erasing during the transfer would stall
// the USB long enough that the connection does not survive it. Can run
// several seconds, past the watchdog, so it is fed once a sector.
bool photoBeginWrite(int slot) {
  if (!validSlot(slot) || photoSlotSpace() < PHOTO_BYTES) return false;
  // Header first, so a broken-off upload cannot leave an old header in
  // front of new data.
  crcChecked[slot] = false;
  crcOk[slot] = false;
  eraseSector(slotFlashOffset(slot));
  for (uint32_t i = 0; i < DATA_SECTORS; i++) {
    eraseSector(slotFlashOffset(slot) + DATA_OFFSET + i * PHOTO_SECTOR);
    videoFeedWatchdog();
  }
  return true;
}

bool photoWriteSector(int slot, uint32_t index, const uint8_t *data) {
  if (!validSlot(slot)) return false;
  // Bounded before the multiplication: index * PHOTO_SECTOR wraps for
  // an index that is a multiple of 2^20.
  if (index >= (SLOT_BYTES - DATA_OFFSET) / PHOTO_SECTOR) return false;
  uint32_t off = index * PHOTO_SECTOR;
  if (off + PHOTO_SECTOR > SLOT_BYTES - DATA_OFFSET) return false;
  programSector(slotFlashOffset(slot) + DATA_OFFSET + off, data, PHOTO_SECTOR);
  return memcmp(photoData(slot) + off, data, PHOTO_SECTOR) == 0;
}

bool photoFinish(int slot, const char *name, uint32_t expectedCrc) {
  if (!validSlot(slot)) return false;
  // Checks the flash only; whether what arrived is what was meant is
  // checked on the PC side, in BoardUpload.Send().
  uint32_t echt = photoCrcFromFlash(slot);
  if (echt != expectedCrc) return false;

  // Static, not on the stack: PICO_STACK_SIZE is only 2 kB.
  static uint8_t sector[PHOTO_SECTOR];
  memset(sector, 0xFF, sizeof(sector));
  PhotoHeader *k = (PhotoHeader *)sector;
  k->magic = PHOTO_MAGIC;
  k->version = 1;
  k->width = 720;
  k->height = 576;
  k->flags = 0;
  k->bytes = PHOTO_BYTES;
  k->crc = echt;
  memset(k->name, 0, sizeof(k->name));
  if (name) strncpy(k->name, name, sizeof(k->name) - 1);

  uint32_t f = slotFlashOffset(slot);
  eraseSector(f);
  programSector(f, sector, PHOTO_SECTOR);
  return photoCheck(slot);
}

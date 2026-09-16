#include "portrait.h"

#include <Arduino.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "photoflash.h"
#include "testcards/testcardf_photo.h"
#include "testcards/testcardw_photo.h"
#include "video.h"

static const uint32_t PORTRAIT_MAGIC = 0x54524F50;  // "PORT"
static const uint32_t PORTRAIT_SECTOR = PHOTO_SECTOR;

static uint32_t targetBytes(int target) {
  if (target == PORTRAIT_TESTCARDF) {
    return testcardf_photo_off[TESTCARDF_PHOTO_ROWS - 1] +
           (uint32_t)testcardf_photo_len[TESTCARDF_PHOTO_ROWS - 1] * 2;
  }
  return testcardw_photo_off[TESTCARDW_PHOTO_ROWS - 1] +
         (uint32_t)testcardw_photo_len[TESTCARDW_PHOTO_ROWS - 1] * 2;
}

static uint32_t dataSectors(int target) {
  return (targetBytes(target) + PORTRAIT_SECTOR - 1) / PORTRAIT_SECTOR;
}

static uint32_t targetSectors(int target) { return 1 + dataSectors(target); }  // header + data

// F's region first, W's right after it; each target() call below walks
// the targets before this one to find its own start.
static uint32_t targetSectorOffset(int target) {
  uint32_t s = 0;
  for (int t = 0; t < target; t++) s += targetSectors(t);
  return s;
}

struct PortraitHeader {
  uint32_t magic;
  uint16_t version;
  uint32_t bytes;  // targetBytes(target) at the time of the upload
  uint32_t crc;    // CRC32 over exactly that many bytes
};

static bool validTarget(int target) { return target >= 0 && target < PORTRAIT_COUNT; }

static const uint8_t *regionBase() {
  return (const uint8_t *)(XIP_BASE + photoFreeOffset());
}

static const uint8_t *headerBase(int target) {
  return regionBase() + targetSectorOffset(target) * PORTRAIT_SECTOR;
}

static uint32_t headerFlashOffset(int target) {
  return (uint32_t)((uintptr_t)headerBase(target) - XIP_BASE);
}

const PortraitHeader *portraitHeader(int target) {
  return (const PortraitHeader *)headerBase(target);
}

const uint8_t *portraitData(int target) { return headerBase(target) + PORTRAIT_SECTOR; }

uint32_t portraitBytes(int target) { return validTarget(target) ? targetBytes(target) : 0; }

uint32_t portraitReservedBytes() {
  uint32_t sectors = 0;
  for (int t = 0; t < PORTRAIT_COUNT; t++) sectors += targetSectors(t);
  return sectors * PORTRAIT_SECTOR;
}

static bool headerOk(int target) {
  if (!validTarget(target)) return false;
  const PortraitHeader *h = portraitHeader(target);
  return h->magic == PORTRAIT_MAGIC && h->bytes == targetBytes(target);
}

// photoCrc32() itself does not invert its result; photoflash.h's own
// photoCrcFromFlash() does that (^ 0xFFFFFFFFu) once, outside it, and
// every caller there goes through that wrapper. This one had not been,
// so it compared and reported the un-inverted value everywhere: still
// internally consistent (finish and check agreed with each other), but
// not the standard zlib-compatible CRC32 the PC side computes with
// BoardUpload.Crc32(), which does invert. That is what "the firmware's
// CRC differs from ours" was, every single time.
uint32_t portraitCrcFromFlash(int target) {
  return photoCrc32(portraitData(target), targetBytes(target), 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
}

// Expensive (up to 136 kB), so run once per target before streaming
// starts; only the cheap header check runs after that. Same reasoning
// as photoflash.h's crcChecked[]/crcOk[].
static bool crcChecked[PORTRAIT_COUNT];
static bool crcOk[PORTRAIT_COUNT];

bool portraitCheck(int target) {
  if (!validTarget(target)) return false;
  crcChecked[target] = true;
  crcOk[target] = headerOk(target) && portraitCrcFromFlash(target) == portraitHeader(target)->crc;
  return crcOk[target];
}

bool portraitValid(int target) {
  return validTarget(target) && headerOk(target) && crcChecked[target] && crcOk[target];
}

static void eraseSector(uint32_t offset) {
  noInterrupts();
  flash_range_erase(offset, PORTRAIT_SECTOR);
  interrupts();
}

static void programSector(uint32_t offset, const uint8_t *data, uint32_t len) {
  noInterrupts();
  flash_range_program(offset, data, len);
  interrupts();
}

bool portraitBeginWrite(int target) {
  if (!validTarget(target)) return false;
  crcChecked[target] = false;
  crcOk[target] = false;
  // Header first, so a broken-off upload cannot leave an old header in
  // front of new data; same order as photoflash.h's photoBeginWrite().
  eraseSector(headerFlashOffset(target));
  for (uint32_t i = 0; i < dataSectors(target); i++) {
    eraseSector(headerFlashOffset(target) + PORTRAIT_SECTOR + i * PORTRAIT_SECTOR);
    videoFeedWatchdog();
  }
  return true;
}

bool portraitWriteSector(int target, uint32_t index, const uint8_t *data) {
  if (!validTarget(target) || index >= dataSectors(target)) return false;
  programSector(headerFlashOffset(target) + PORTRAIT_SECTOR + index * PORTRAIT_SECTOR, data,
               PORTRAIT_SECTOR);
  return memcmp(portraitData(target) + index * PORTRAIT_SECTOR, data, PORTRAIT_SECTOR) == 0;
}

bool portraitFinish(int target, uint32_t expectedCrc) {
  if (!validTarget(target)) return false;
  uint32_t echt = portraitCrcFromFlash(target);
  if (echt != expectedCrc) return false;

  // One flash page, not the whole sector: flash_range_program() only
  // demands 256-byte page alignment (SDK hardware/flash.h), and the
  // struct is nowhere near that; the rest of the sector stays the
  // erase's own 0xFF, on the stack safely (2 kB to spare, unlike the
  // uploadBlock-sized sector buffer photoFinish() needs for its whole,
  // fully used PhotoHeader-sized-and-then-some sector).
  uint8_t page[FLASH_PAGE_SIZE];
  memset(page, 0xFF, sizeof(page));
  PortraitHeader *h = (PortraitHeader *)page;
  h->magic = PORTRAIT_MAGIC;
  h->version = 1;
  h->bytes = targetBytes(target);
  h->crc = echt;

  uint32_t f = headerFlashOffset(target);
  eraseSector(f);
  programSector(f, page, sizeof(page));
  return portraitCheck(target);
}

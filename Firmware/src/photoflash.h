// Fifteen replaceable pictures in a reserved area of flash. Raw, fixed
// sector boundaries, no filesystem: the render loop memcpy's a row
// straight out of XIP space at 22.5 MB/s (see CFG_FLASH_CLKDIV in
// config.h), which LittleFS cannot guarantee. Every slot is 204 sectors
// of 4096 bytes: sector 0 the header below, sectors 1..203 the picture
// (576 rows of 1440 bytes). Slot 0 is Custom 1, so the number shown in
// the menu and at upload is one higher than the index used here. The
// header is written last, so a broken-off upload leaves an empty slot
// rather than half a picture.
#ifndef PHOTOFLASH_H
#define PHOTOFLASH_H

#include <stdint.h>

static const uint32_t PHOTO_MAGIC = 0x4F544F46;  // "FOTO"
static const uint32_t PHOTO_SECTOR = 4096;
static const uint32_t PHOTO_BYTES = 720u * 576u * 2u;  // 829440, 4:2:2

// A property of the board, not a taste: every slot is 204 sectors
// (835584 bytes), and the settings need one sector after the last of
// them. photoSlotSpace() returns 0 when the platformio.ini reservation
// is too small for this, so a wrong value shows up at boot instead of
// quietly writing over something else.
//
// Fifteen, then fourteen: the fifteenth slot's own flash now holds the
// two custom BBC Test Card F/W photo replacements instead (see
// portrait.h), 258048 of its 835584 bytes; settings.cpp's journal keeps
// the rest.
static const int PHOTO_SLOTS = 14;

struct PhotoHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t width;
  uint16_t height;
  uint16_t flags;
  uint32_t bytes;
  uint32_t crc;  // CRC32 over the picture data
  char name[32];
};

// Cheap: only looks at the header and the last photoCheck() result.
// Safe from a render loop or an init.
bool photoValid(int slot);
bool photoHeaderOk(int slot);   // header only, no CRC

// THE EXPENSIVE CHECK: CRC32 over all 829440 bytes, about half a second
// per filled slot. DO NOT CALL WHILE PICTURE IS GOING OUT, it takes many
// times longer than the 4 ms ring buffer and core0 builds no rows
// meanwhile. Meant for startup, before the producer loop runs;
// photoFinish() also calls it after an upload.
bool photoCheck(int slot);

const PhotoHeader *photoHeader(int slot);   // only meaningful if photoValid()

// A read pointer into XIP space; always a valid address, use
// photoValid() to know if it means anything.
const uint8_t *photoData(int slot);

uint32_t photoSlotSpace();   // smaller than PHOTO_BYTES means the reservation is wrong

// WRITING. core1 must already be parked with videoSuspend() before any
// of these three, or it fetches instructions from the flash being
// erased here and hangs. No video and no USB while writing, since
// interrupts are off; the sender waits for an ack per sector. Order:
// videoSuspend() -> photoBeginWrite() -> photoWriteSector() per sector
// -> photoFinish(), which writes the header and makes the slot valid.
bool photoBeginWrite(int slot);
bool photoWriteSector(int slot, uint32_t index, const uint8_t *data);
bool photoFinish(int slot, const char *name, uint32_t expectedCrc);

// Reflected CRC32, polynomial 0xEDB88320, so the PC side can match it
// with zlib.crc32().
uint32_t photoCrc32(const uint8_t *data, uint32_t len, uint32_t crc);

// The space left over after the photograph slots; settings.cpp keeps
// the settings in its first sector. Changing PHOTO_SLOTS or the
// reservation moves that sector, so saved settings are gone once.
uint32_t photoFreeOffset();  // flash offset from the start of the chip
uint32_t photoFreeBytes();
uint32_t photoReservedBytes();   // the whole reserved area, _FS_end - _FS_start

uint32_t photoCrcFromFlash(int slot);
#endif  // PHOTOFLASH_H

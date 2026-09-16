// User-uploaded replacements for the photo window of BBC Test Card F
// and BBC Test Card W. Packed in exactly the same ragged, per-row shape
// as the built-in testcardf_photo[]/testcardw_photo[] arrays (see
// testcards/testcardf_photo.h and testcards/testcardw_photo.h): row r
// holds *_photo_len[r] samples at byte offset *_photo_off[r]. The two
// cards' oval windows differ in size (302 rows/135784 bytes for F, 310
// rows/107120 bytes for W), so this holds one packed blob per card, not
// one shared picture.
//
// Lives right after the Custom photo slots, in the flash PHOTO_SLOTS
// gave up going from 15 to 14 (see photoflash.h); portraitReservedBytes()
// tells settings.cpp's journal where its own free space now starts.
#ifndef PORTRAIT_H
#define PORTRAIT_H

#include <stdint.h>

static const int PORTRAIT_TESTCARDF = 0;
static const int PORTRAIT_TESTCARDW = 1;
static const int PORTRAIT_COUNT = 2;

// Cheap: header and CRC flag only, safe from a render loop.
bool portraitValid(int target);

// The expensive check, CRC32 over the packed data; call once at
// startup, the same rule as photoCheck() in photoflash.h.
bool portraitCheck(int target);

// A read pointer into XIP space; always valid, use portraitValid() to
// know if it means anything.
const uint8_t *portraitData(int target);

// The exact packed size for this target (testcardf_photo_off[ROWS-1] +
// testcardf_photo_len[ROWS-1]*2, or testcardw's own), not the
// sector-rounded space reserved for it.
uint32_t portraitBytes(int target);

// The standard (inverted) CRC32 over the target's own bytes, straight
// out of flash; zlib- and BoardUpload.Crc32()-compatible, the same rule
// photoCrcFromFlash() follows in photoflash.h.
uint32_t portraitCrcFromFlash(int target);

// How much flash this module claims in total, right after the photo
// slots; settings.cpp's journal starts its own free space after this.
uint32_t portraitReservedBytes();

// WRITING. Same calling convention as photoflash.h: videoSuspend()
// first, then portraitBeginWrite() -> portraitWriteSector() per sector
// (always a full PHOTO_SECTOR, the caller zero-pads the last one) ->
// portraitFinish(), which writes the header and makes the target valid.
bool portraitBeginWrite(int target);
bool portraitWriteSector(int target, uint32_t index, const uint8_t *data);
bool portraitFinish(int target, uint32_t expectedCrc);

#endif  // PORTRAIT_H

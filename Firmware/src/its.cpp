#include "its.h"

#include <stdio.h>
#include <string.h>

#include <pico.h>   // __not_in_flash_func
#include "config.h"
#include "its_data.h"

static bool on = CFG_ITS;

void itsSetEnabled(bool v) { on = v; }
bool itsEnabled() { return on; }

bool itsClaimsLine(uint32_t line) {
  if (!on) return false;
  for (int i = 0; i < ITS_COUNT; i++) {
    if ((uint32_t)ITS_LINES[i] == line) return true;
  }
  return false;
}

// IN RAM: four of these lines go out every frame, and while the 1440
// bytes copied still come from flash, the code itself is not fetched
// again behind an eviction.
bool __not_in_flash_func(itsRenderLine)(uint8_t *uyvy, uint32_t line) {
  if (!on) return false;
  for (int i = 0; i < ITS_COUNT; i++) {
    if ((uint32_t)ITS_LINES[i] == line) {
      // Word by word, not memcpy(): the library one lives in flash and
      // would pull the XIP cache back into a RAM routine's way.
      const uint32_t *src = (const uint32_t *)ITS_DATA[i];
      uint32_t *dst = (uint32_t *)uyvy;
      for (int k = 0; k < 1440 / 4; k++) dst[k] = src[k];
      return true;
    }
  }
  return false;
}

void itsStatus(char *buf, unsigned n) {
  if (!on) {
    snprintf(buf, n, "off");
    return;
  }
  snprintf(buf, n, "on, lines %d, %d, %d and %d", ITS_LINES[0], ITS_LINES[1], ITS_LINES[2],
           ITS_LINES[3]);
}

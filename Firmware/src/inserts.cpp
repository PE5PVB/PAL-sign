#include "inserts.h"

#include "clock.h"
#include "config.h"
#include "settings.h"

// Shared by the G00 and the G924: both have insert fields (the G00
// draws a black box, the G924 a thin line). Text is laid out again once
// the second ticks over, not every picture; a fixed mode happens once,
// guarded by resetNeeded.
static int64_t lastShown = -1;
static bool resetNeeded = true;

void insertForceRefresh() {
  lastShown = -1;
  resetNeeded = true;
}

static char *put2(char *d, int v) {
  *d++ = (char)('0' + (v / 10) % 10);
  *d++ = (char)('0' + v % 10);
  return d;
}

static bool inUse = false;

bool insertInUse() { return inUse; }
void insertClearInUse() { inUse = false; }

// Where the boxes are, caught as they go past; the PC tool asks for
// these to put a running clock in the picture it keeps.
static int boxLx0, boxLx1, boxRx0, boxRx1, boxY0, boxY1;

void insertGetBoxes(int *lx0, int *lx1, int *rx0, int *rx1, int *y0, int *y1) {
  *lx0 = boxLx0;
  *lx1 = boxLx1;
  *rx0 = boxRx0;
  *rx1 = boxRx1;
  *y0 = boxY0;
  *y1 = boxY1;
}

// Leaves the text out while the PC is fetching a picture; the boxes
// themselves stay.
static bool blank = false;
void insertSetBlank(bool on) {
  blank = on;
  resetNeeded = true;
  lastShown = -1;
}

bool insertPrepare(TextOverlay *left, TextOverlay *right, int lx0, int lx1, int rx0, int rx1,
                   int y0, int y1) {
  inUse = true;
  boxLx0 = lx0;
  boxLx1 = lx1;
  boxRx0 = rx0;
  boxRx1 = rx1;
  boxY0 = y0;
  boxY1 = y1;
  int mode = insertGetMode();
  if (blank && (mode == INSERT_TIME || mode == INSERT_DATE_TIME)) {
    if (!resetNeeded) return false;
    resetNeeded = false;
    left->count = 0;
    right->count = 0;
    return true;
  }
  bool wantsDate = (mode == INSERT_DATE_TIME);
  bool wantsTime = (mode == INSERT_DATE_TIME || mode == INSERT_TIME);

  if (!wantsDate && !wantsTime) {
    if (!resetNeeded) return false;
    resetNeeded = false;
    if (mode == INSERT_TEXT) {
      textPrepare(left, cfgInsertLeft(), lx0, lx1, y0, y1);
      textPrepare(right, cfgInsertRight(), rx0, rx1, y0, y1);
    } else {
      left->count = 0;
      right->count = 0;
    }
    return true;
  }

  ClockTime t;
  int64_t nu = clockGet(&t);
  if (nu == lastShown) return false;
  lastShown = nu;
  resetNeeded = false;

  char date[12], time[12], *d = date;
  if (CFG_DATE_FORMAT == 2) {
    d = put2(d, t.year / 100);
    d = put2(d, t.year);
    *d++ = '-';
    d = put2(d, t.month);
    *d++ = '-';
    d = put2(d, t.day);
  } else {
    d = put2(d, t.day);
    *d++ = '-';
    d = put2(d, t.month);
    *d++ = '-';
    if (CFG_DATE_FORMAT == 1) d = put2(d, t.year / 100);
    d = put2(d, t.year);
  }
  *d = 0;

  char *k = time;
  k = put2(k, t.hour);
  *k++ = ':';
  k = put2(k, t.min);
  *k++ = ':';
  k = put2(k, t.sec);
  *k = 0;

  if (wantsDate) {
    textPrepare(left, date, lx0, lx1, y0, y1);
  } else {
    left->count = 0;
  }
  if (wantsTime) {
    textPrepare(right, time, rx0, rx1, y0, y1);
  } else {
    right->count = 0;
  }
  return true;
}

#include "clock.h"

#include <pico/time.h>

#include "config.h"
#include "rv3028.h"
#include "settings.h"

// Seconds since 1970, anchored to the chip's microsecond counter.
// epochBase always holds standard time, EU summer time is added on read
// (dstActiveStandard() below).
static int64_t epochBase = 0;
static uint64_t usBase = 0;

// Calendar date <-> day number, Howard Hinnant's algorithm.
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static void civilFromDays(int64_t z, int *y, int *m, int *d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365u * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5u * doy + 2) / 153;
  const unsigned dd = doy - (153u * mp + 2) / 5 + 1;
  const unsigned mm = mp + (mp < 10 ? 3 : -9);
  *y = (int)((int64_t)yoe + era * 400 + (mm <= 2 ? 1 : 0));
  *m = (int)mm;
  *d = (int)dd;
}

// EU summer time (Directive 2000/84/EC): 01:00 UTC on the last Sunday
// of March and October, 02:00 standard time here (fixed CET offset).
static int weekdayOf(int64_t days) {
  // day 0 (1970-01-01) was a Thursday; 0 returned means Sunday.
  return (int)(((days % 7) + 7 + 4) % 7);
}

static int64_t lastSundayOfMonth(int year, int month) {
  static const int8_t lastDay[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int64_t last = daysFromCivil(year, month, lastDay[month]);
  return last - weekdayOf(last);
}

// True if EU summer time applies at standard-time instant e (seconds
// since 1970).
static bool dstActiveStandard(int64_t e) {
  int64_t days = e / 86400;
  if (e % 86400 < 0) days--;   // C truncates towards zero, this floors
  int y, m, d;
  civilFromDays(days, &y, &m, &d);
  const int64_t start = lastSundayOfMonth(y, 3) * 86400 + 2 * 3600;
  const int64_t end = lastSundayOfMonth(y, 10) * 86400 + 2 * 3600;
  return e >= start && e < end;
}

static void anchor(int64_t e) {
  epochBase = e;
  usBase = time_us_64();
}

// Wall clock fields to standard-time epoch: what is typed is the
// current wall clock, so the DST hour comes off before storing.
static int64_t standardEpochOf(int year, int month, int day, int hour, int min, int sec) {
  int64_t e = daysFromCivil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
  if (cfgDstAutoOn() && dstActiveStandard(e)) e -= 3600;
  return e;
}

void clockSet(int year, int month, int day, int hour, int min, int sec) {
  const int64_t e = standardEpochOf(year, month, day, hour, min, sec);
  anchor(e);
  // The RTC keeps standard time; the DST hour goes back on at read.
  if (rv3028Present()) {
    const int64_t days = e / 86400;
    int y, m, d;
    civilFromDays(days, &y, &m, &d);
    const int64_t rem = e - days * 86400;
    rv3028WriteTime(y, m, d, weekdayOf(days), (int)(rem / 3600), (int)((rem / 60) % 60),
                    (int)(rem % 60), e);
  }
}

bool clockBegin() {
  rv3028Begin();
  int64_t e;
  if (rv3028ReadTime(&e)) {
    anchor(e);
    return true;
  }
  // Straight to anchor(), not through clockSet(): a factory-fresh RTC
  // keeps its power-lost flag until someone really sets the time, so
  // the config.h start values never masquerade as a kept time.
  anchor(standardEpochOf(CFG_CLOCK_START_YEAR, CFG_CLOCK_START_MONTH, CFG_CLOCK_START_DAY,
                         CFG_CLOCK_START_HOUR, CFG_CLOCK_START_MIN, CFG_CLOCK_START_SEC));
  return false;
}

// The RTC is the timekeeper: this crystal drifts a few seconds a day,
// so once an hour the clock is pulled back onto the RTC. The 2 s dead
// band keeps the on-screen seconds from twitching on every sync.
void clockService() {
  if (!rv3028Present()) return;
  static uint64_t lastUs = 0;
  const uint64_t now = time_us_64();
  if (now - lastUs < 3600000000ull) return;
  lastUs = now;
  int64_t e;
  if (!rv3028ReadTime(&e)) return;
  const int64_t local = epochBase + (int64_t)((time_us_64() - usBase) / 1000000u);
  if (e - local >= 2 || local - e >= 2) anchor(e);
}

int64_t clockGet(ClockTime *t) {
  int64_t e = epochBase + (int64_t)((time_us_64() - usBase) / 1000000u);
  if (cfgDstAutoOn() && dstActiveStandard(e)) e += 3600;
  int64_t days = e / 86400;
  int64_t rem = e % 86400;
  if (rem < 0) {  // C truncates towards zero, we want to round down
    rem += 86400;
    days--;
  }
  civilFromDays(days, &t->year, &t->month, &t->day);
  t->hour = (int)(rem / 3600);
  t->min = (int)((rem / 60) % 60);
  t->sec = (int)(rem % 60);
  return e;
}

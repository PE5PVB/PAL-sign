// Calendar date <-> day number since 1970-01-01, Howard Hinnant's
// algorithms. Shared by the clock and the RTC driver.
#ifndef CIVIL_H
#define CIVIL_H

#include <stdint.h>

static inline int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static inline void civilFromDays(int64_t z, int *y, int *m, int *d) {
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
#endif  // CIVIL_H

// Running date and time for the test pattern. On a board with an
// RV-3028 RTC (rv3028.h) that chip is the timekeeper: loaded at boot,
// written on every set, and re-anchored hourly. Without one the clock
// starts at config.h's values and runs on the system clock alone,
// drifting a few seconds a day (same crystal as the pixel clock).
// Settable over serial (key T) or the on-device System menu.
#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

struct ClockTime {
  int year;   // 1970..
  int month;  // 1..12
  int day;    // 1..31
  int hour;   // 0..23
  int min;    // 0..59
  int sec;    // 0..59
};

// Probes the RTC and starts the clock from it, or from config.h's
// values when it is absent or was never set. True = time came from
// the RTC. Call once from setup(), after the I2C bus is running.
bool clockBegin();

// The hourly RTC re-anchor, self-throttled; call it from loop() when
// the row stock can spare an I2C read. A no-op without an RTC.
void clockService();

void clockSet(int year, int month, int day, int hour, int min, int sec);

// Return value is the same moment in seconds since 1970, for a one line
// comparison to detect a change.
int64_t clockGet(ClockTime *t);
#endif  // CLOCK_H

// Real-time clock on the I2C bus, fitted on boards that have one:
// probed once at boot, like the front panel and the ADC. Two Micro
// Crystal parts are supported, the RV-3028-C7 (address 52h) and the
// RV-3032-C7 (address 51h); the first one that answers is used. Both
// keep standard time (never DST), and a backup cell on VBACKUP carries
// that through power cuts.
#ifndef RTC_H
#define RTC_H

#include <stdint.h>

// Probes both chips and, when one is found, puts its backup switchover
// and trickle charger right (a one-time EEPROM write, skipped when the
// stored configuration already matches). Call once from setup(), after
// the I2C bus is running.
void rtcBegin();

bool rtcPresent();

// "RV-3028-C7", "RV-3032-C7", or "none".
const char *rtcName();

// Standard-time seconds since 1970. False when no chip is fitted, when
// power was ever lost beyond the backup (PORF, and VLF on the RV-3032),
// or when the value cannot be a real time.
bool rtcReadTime(int64_t *epoch);

// Writes the calendar (and on the RV-3028 the UNIX counter), standard
// time, and clears the power-lost flags. Years outside 2000..2099 do
// not fit the calendar and are refused whole.
void rtcWriteTime(int year, int month, int day, int weekday,
                  int hour, int min, int sec, int64_t epoch);

// Status register and the EEPROM backup/PMU mirror, for the boot
// report: the status says whether the time survived, the backup byte
// says whether the switchover/trickle configuration really reached the
// EEPROM. RV-3028: Status 0Eh and EEPROM Backup 37h; RV-3032: Status
// 0Dh and EEPROM PMU C0h. False when no chip answers.
bool rtcPeek(uint8_t *status, uint8_t *backup);
#endif  // RTC_H

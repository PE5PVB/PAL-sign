// RV-3028-C7 real-time clock on the I2C bus, fitted on boards that
// have one: probed once at boot, like the front panel and the ADC.
// It keeps standard time (never DST) in its 32-bit UNIX counter, and
// a supercap on VBACKUP carries that through power cuts.
#ifndef RV3028_H
#define RV3028_H

#include <stdint.h>

// Probes the chip and, when found, puts the backup switchover and the
// trickle charger right (a one-time EEPROM write, skipped when the
// stored configuration already matches). Call once from setup(),
// after the I2C bus is running.
void rv3028Begin();

bool rv3028Present();

// Standard-time seconds since 1970 from the UNIX counter. False when
// the chip is absent, when power was ever lost beyond the backup
// (PORF), or when the value cannot be a real time.
bool rv3028ReadTime(int64_t *epoch);

// Writes the calendar and the UNIX counter (both standard time) and
// clears PORF. Years outside 2000..2099 do not fit the calendar and
// are refused whole.
void rv3028WriteTime(int year, int month, int day, int weekday,
                     int hour, int min, int sec, int64_t epoch);

// Status (0Eh) and the EEPROM Backup mirror (37h), for the boot
// report: PORF in the status says whether the time survived, the
// backup byte says whether the switchover/trickle configuration
// really reached the EEPROM. False when the chip does not answer.
bool rv3028Peek(uint8_t *status, uint8_t *backup);
#endif  // RV3028_H

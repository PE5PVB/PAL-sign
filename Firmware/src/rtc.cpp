// RTC driver for the RV-3028-C7 and the RV-3032-C7, the minimum this
// firmware needs: presence, the time, and the backup configuration
// for the cell on VBACKUP. The two chips share the EEPROM mechanics
// (a RAM mirror, an Update command, EERD to hold off the automatic
// refresh, an EEbusy bit) but at different addresses, so each gets a
// register table and the routines are written once.
//
// RV-3028-C7, Application Manual Rev. 1.4 (Nov 2021): slave address
// 1010010b (5.6); UNIX Time at 1Bh..1Eh LSB first, read twice and
// compare since the four bytes are not latched (4.16.2); writing the
// Seconds register resets the prescaler so the second starts at the
// write, and the UNIX write must follow within 950 ms (4.16.1/4.17);
// Status 0Eh bit 0 PORF, bit 7 EEbusy; Control 1 0Fh bit 3 EERD; EE
// Command 27h wants 00h before 11h (update RAM mirror to EEPROM);
// EEPROM Backup 37h holds the factory offset LSB in bit 7 (preserve
// it), TCE bit 5, FEDE bit 4 (always 1), BSM bits 3:2 (11 = level
// switching) and TCR bits 1:0 (00 = 3 kOhm). Trickle charger on,
// fed from VDD through the 3 kOhm resistor and a Schottky diode (4.3).
//
// RV-3032-C7, Application Manual Rev. 1.3 (May 2023): slave address
// 1010001b (51h); 00h is 100th seconds, the calendar is 01h Seconds
// to 07h Year, BCD, hours 00..23 only, weekday 0..6 (3.3/3.4); no
// UNIX counter, so the seven bytes are read twice and compared; a
// write to Seconds resets the prescaler (TIME SYNCHRONIZATION);
// Status 0Dh bit 1 PORF (set at power-up), bit 0 VLF (internal supply
// fell below 1.2 V, time no longer trusted), both cleared by writing
// 0; Temperature LSBs 0Eh bit 2 EEbusy; Control 1 10h bit 2 EERD; EE
// Command 3Fh takes 11h alone (4.6.3, ~46 ms); EEPROM PMU C0h: bit 6
// NCLKE (1 = CLKOUT low, the pin is unused on this board), BSM bits
// 5:4, TCR bits 3:2, TCM bits 1:0 (4.2/4.3). Direct switching (BSM
// 01) with the charger fed straight from VDD (TCM 01): the manual
// names exactly this case for DSM, a rechargeable source charged
// through the internal resistor sits below VDD by the diode drop
// (4.2.2). TCR 01 = 2 kOhm (1.6..2.2 kOhm) plus the 1 kOhm on the
// board keeps the charge current of the ML2032 under 0.4 mA at an
// empty cell; Maxell allows 2 mA and a charge voltage up to 3.3 V,
// the cell sees VDD minus the 0.25 V diode.
#include "rtc.h"

#include <Arduino.h>
#include <Wire.h>
#include <string.h>

#include "civil.h"

enum Chip { NONE, RV3028, RV3032 };

struct Regs {
  uint8_t addr;
  uint8_t status;       // register with the power-lost flags
  uint8_t lostMask;     // flags in it that mean "time not kept"
  uint8_t busyReg;      // register with EEbusy
  uint8_t busyMask;
  uint8_t ctrl1;
  uint8_t eerdMask;
  uint8_t eecmd;
  bool eecmdZeroFirst;  // RV-3028 wants 00h before the 11h command
  uint8_t backup;       // EEPROM mirror with switchover and charger
  uint8_t backupKeep;   // bits in it to preserve (factory data)
  uint8_t backupWant;
  const char *name;
};

static const Regs RV3028_REGS = {0x52, 0x0E, 0x01, 0x0E, 0x80, 0x0F, 0x08, 0x27, true,
                                 0x37, 0x80, 0x3C,   // TCE | FEDE | BSM 11 | TCR 00
                                 "RV-3028-C7"};
static const Regs RV3032_REGS = {0x51, 0x0D, 0x03, 0x0E, 0x04, 0x10, 0x04, 0x3F, false,
                                 0xC0, 0x00, 0x55,   // NCLKE | BSM 01 | TCR 01 | TCM 01
                                 "RV-3032-C7"};

static const uint8_t RV3028_REG_SECONDS = 0x00;
static const uint8_t RV3028_REG_UNIX0 = 0x1B;
static const uint8_t RV3032_REG_SECONDS = 0x01;

static Chip chip = NONE;
static const Regs *R = nullptr;

static bool rdRegs(uint8_t reg, uint8_t *v, int n) {
  Wire.beginTransmission(R->addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)R->addr, n) != n) return false;
  for (int i = 0; i < n; i++) v[i] = (uint8_t)Wire.read();
  return true;
}

static bool wrRegs(uint8_t reg, const uint8_t *v, int n) {
  Wire.beginTransmission(R->addr);
  Wire.write(reg);
  for (int i = 0; i < n; i++) Wire.write(v[i]);
  return Wire.endTransmission() == 0;
}

static bool eeIdle() {
  const uint32_t t0 = millis();
  while (millis() - t0 < 250) {
    uint8_t s;
    if (rdRegs(R->busyReg, &s, 1) && !(s & R->busyMask)) return true;
    delay(1);
  }
  return false;
}

// One EEPROM cycle in the device's life, not one per boot: skipped as
// soon as the stored configuration matches.
static void configureBackup() {
  uint8_t cur;
  if (!rdRegs(R->backup, &cur, 1)) return;
  const uint8_t want = (uint8_t)((cur & R->backupKeep) | R->backupWant);
  if (cur == want) return;
  uint8_t c1;
  if (!rdRegs(R->ctrl1, &c1, 1)) return;
  // EERD first, EEbusy after: checked the other way around, an
  // automatic refresh can slip in between and the update fails.
  uint8_t hold = (uint8_t)(c1 | R->eerdMask);
  wrRegs(R->ctrl1, &hold, 1);
  if (!eeIdle()) return;
  wrRegs(R->backup, &want, 1);
  uint8_t cmd = 0x00;
  if (R->eecmdZeroFirst) wrRegs(R->eecmd, &cmd, 1);
  cmd = 0x11;
  wrRegs(R->eecmd, &cmd, 1);
  eeIdle();
  uint8_t release = (uint8_t)(c1 & ~R->eerdMask);
  wrRegs(R->ctrl1, &release, 1);
}

static bool answers(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void rtcBegin() {
  chip = NONE;
  R = nullptr;
  if (answers(RV3028_REGS.addr)) {
    chip = RV3028;
    R = &RV3028_REGS;
  } else if (answers(RV3032_REGS.addr)) {
    chip = RV3032;
    R = &RV3032_REGS;
  }
  if (chip != NONE) configureBackup();
}

bool rtcPresent() { return chip != NONE; }

const char *rtcName() { return chip == NONE ? "none" : R->name; }

static int fromBcd(uint8_t v) { return ((v >> 4) & 0x0F) * 10 + (v & 0x0F); }

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// Below 2020-01-01 the counter never held a real time.
static const int64_t EPOCH_FLOOR = 1577836800;

static bool readUnix(int64_t *epoch) {
  uint8_t a[4], b[4];
  for (int tries = 0; tries < 3; tries++) {
    if (!rdRegs(RV3028_REG_UNIX0, a, 4) || !rdRegs(RV3028_REG_UNIX0, b, 4)) return false;
    if (memcmp(a, b, 4) != 0) continue;   // a 1 Hz tick fell in between
    const uint32_t u = (uint32_t)a[0] | ((uint32_t)a[1] << 8) |
                       ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24);
    if (u < EPOCH_FLOOR) return false;
    *epoch = (int64_t)u;
    return true;
  }
  return false;
}

static bool readCalendar(int64_t *epoch) {
  uint8_t a[7], b[7];
  for (int tries = 0; tries < 3; tries++) {
    if (!rdRegs(RV3032_REG_SECONDS, a, 7) || !rdRegs(RV3032_REG_SECONDS, b, 7)) return false;
    if (memcmp(a, b, 7) != 0) continue;   // a 1 Hz tick fell in between
    const int sec = fromBcd(a[0] & 0x7F), min = fromBcd(a[1] & 0x7F);
    const int hour = fromBcd(a[2] & 0x3F), day = fromBcd(a[4] & 0x3F);
    const int month = fromBcd(a[5] & 0x1F), year = 2000 + fromBcd(a[6]);
    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || month < 1 || month > 12) {
      return false;
    }
    const int64_t e = daysFromCivil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
    if (e < EPOCH_FLOOR) return false;
    *epoch = e;
    return true;
  }
  return false;
}

bool rtcReadTime(int64_t *epoch) {
  if (chip == NONE) return false;
  uint8_t s;
  if (!rdRegs(R->status, &s, 1)) return false;
  if (s & R->lostMask) return false;
  return chip == RV3028 ? readUnix(epoch) : readCalendar(epoch);
}

bool rtcPeek(uint8_t *status, uint8_t *backup) {
  if (chip == NONE) return false;
  return rdRegs(R->status, status, 1) && rdRegs(R->backup, backup, 1);
}

void rtcWriteTime(int year, int month, int day, int weekday,
                  int hour, int min, int sec, int64_t epoch) {
  if (chip == NONE) return;
  if (year < 2000 || year > 2099) return;
  const uint8_t cal[7] = {bcd(sec),  bcd(min),   bcd(hour), (uint8_t)weekday,
                          bcd(day),  bcd(month), bcd(year - 2000)};
  if (!wrRegs(chip == RV3028 ? RV3028_REG_SECONDS : RV3032_REG_SECONDS, cal, 7)) return;
  if (chip == RV3028) {
    const uint8_t ux[4] = {(uint8_t)epoch, (uint8_t)(epoch >> 8),
                           (uint8_t)(epoch >> 16), (uint8_t)(epoch >> 24)};
    wrRegs(RV3028_REG_UNIX0, ux, 4);
  }
  const uint8_t clear = 0x00;   // PORF (and VLF) included: the time is real now
  wrRegs(R->status, &clear, 1);
}

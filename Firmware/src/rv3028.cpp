// RV-3028-C7 driver, the minimum this firmware needs: presence, the
// UNIX counter, and the backup configuration for the supercap. All
// register facts from the Application Manual Rev. 1.4 (Nov 2021):
// slave address 1010010b (5.6); UNIX Time at 1Bh..1Eh LSB first, read
// twice and compare since the four bytes are not latched (4.16.2);
// writing the Seconds register resets the prescaler so the second
// starts at the write, and the UNIX write must follow within 950 ms
// (4.16.1/4.17); Status 0Eh bit 0 PORF, bit 7 EEbusy; Control 1 0Fh
// bit 3 EERD; EE Command 27h wants 00h before 11h (update RAM mirror
// to EEPROM); EEPROM Backup 37h holds the factory offset LSB in bit 7
// (preserve it), TCE bit 5, FEDE bit 4 (always 1), BSM bits 3:2 and
// TCR bits 1:0. Level switching (BSM 11) and not direct: with VDD and
// VBACKUP both near 3.3 V the manual calls DSM not recommended
// (4.2.2). Trickle charger on at 3 kOhm feeds the supercap (4.3).
#include "rv3028.h"

#include <Arduino.h>
#include <Wire.h>
#include <string.h>

static const uint8_t RV_ADDR = 0x52;
static const uint8_t REG_SECONDS = 0x00;
static const uint8_t REG_STATUS = 0x0E;
static const uint8_t REG_CTRL1 = 0x0F;
static const uint8_t REG_UNIX0 = 0x1B;
static const uint8_t REG_EECMD = 0x27;
static const uint8_t REG_BACKUP = 0x37;

static const uint8_t STATUS_PORF = 0x01;
static const uint8_t STATUS_EEBUSY = 0x80;
static const uint8_t CTRL1_EERD = 0x08;
// TCE | FEDE | BSM = 11 (level switching) | TCR = 00 (3 kOhm).
static const uint8_t BACKUP_WANTED = 0x3C;

static bool present = false;

static bool rdRegs(uint8_t reg, uint8_t *v, int n) {
  Wire.beginTransmission(RV_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)RV_ADDR, n) != n) return false;
  for (int i = 0; i < n; i++) v[i] = (uint8_t)Wire.read();
  return true;
}

static bool wrRegs(uint8_t reg, const uint8_t *v, int n) {
  Wire.beginTransmission(RV_ADDR);
  Wire.write(reg);
  for (int i = 0; i < n; i++) Wire.write(v[i]);
  return Wire.endTransmission() == 0;
}

static bool eeIdle() {
  const uint32_t t0 = millis();
  while (millis() - t0 < 250) {
    uint8_t s;
    if (rdRegs(REG_STATUS, &s, 1) && !(s & STATUS_EEBUSY)) return true;
    delay(1);
  }
  return false;
}

// One EEPROM cycle in the device's life, not one per boot: skipped as
// soon as the stored configuration matches.
static void configureBackup() {
  uint8_t cur;
  if (!rdRegs(REG_BACKUP, &cur, 1)) return;
  const uint8_t want = (uint8_t)((cur & 0x80) | BACKUP_WANTED);
  if (cur == want) return;
  uint8_t c1;
  if (!rdRegs(REG_CTRL1, &c1, 1)) return;
  // EERD first, EEbusy after: checked the other way around, an
  // automatic refresh can slip in between and the update fails.
  uint8_t hold = (uint8_t)(c1 | CTRL1_EERD);
  wrRegs(REG_CTRL1, &hold, 1);
  if (!eeIdle()) return;
  wrRegs(REG_BACKUP, &want, 1);
  uint8_t cmd = 0x00;
  wrRegs(REG_EECMD, &cmd, 1);
  cmd = 0x11;
  wrRegs(REG_EECMD, &cmd, 1);
  eeIdle();
  uint8_t release = (uint8_t)(c1 & ~CTRL1_EERD);
  wrRegs(REG_CTRL1, &release, 1);
}

void rv3028Begin() {
  Wire.beginTransmission(RV_ADDR);
  present = Wire.endTransmission() == 0;
  if (present) configureBackup();
}

bool rv3028Present() { return present; }

bool rv3028ReadTime(int64_t *epoch) {
  if (!present) return false;
  uint8_t s;
  if (!rdRegs(REG_STATUS, &s, 1)) return false;
  if (s & STATUS_PORF) return false;
  uint8_t a[4], b[4];
  for (int tries = 0; tries < 3; tries++) {
    if (!rdRegs(REG_UNIX0, a, 4) || !rdRegs(REG_UNIX0, b, 4)) return false;
    if (memcmp(a, b, 4) != 0) continue;   // a 1 Hz tick fell in between
    const uint32_t u = (uint32_t)a[0] | ((uint32_t)a[1] << 8) |
                       ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24);
    // Below 2020-01-01 the counter never held a real time.
    if (u < 1577836800u) return false;
    *epoch = (int64_t)u;
    return true;
  }
  return false;
}

bool rv3028Peek(uint8_t *status, uint8_t *backup) {
  if (!present) return false;
  return rdRegs(REG_STATUS, status, 1) && rdRegs(REG_BACKUP, backup, 1);
}

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

void rv3028WriteTime(int year, int month, int day, int weekday,
                     int hour, int min, int sec, int64_t epoch) {
  if (!present) return;
  if (year < 2000 || year > 2099) return;
  const uint8_t cal[7] = {bcd(sec),  bcd(min),   bcd(hour), (uint8_t)weekday,
                          bcd(day),  bcd(month), bcd(year - 2000)};
  if (!wrRegs(REG_SECONDS, cal, 7)) return;
  const uint8_t ux[4] = {(uint8_t)epoch, (uint8_t)(epoch >> 8),
                         (uint8_t)(epoch >> 16), (uint8_t)(epoch >> 24)};
  wrRegs(REG_UNIX0, ux, 4);
  const uint8_t clear = 0x00;   // PORF included: the time is real now
  wrRegs(REG_STATUS, &clear, 1);
}

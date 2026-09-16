// Clock speed of the QSPI flash: clk_sys / CLKDIV in QMI M0_TIMING (odd
// and even divisors allowed, reset 4, 256 encoded as 0). Four bits a
// clock, so the theoretical rate in MB/s is half the SCK frequency in
// MHz. Divisor is set in config.h at CFG_FLASH_CLKDIV.
//
// The divider changes while running and flash is briefly unreadable
// meanwhile, so everything here runs from RAM, and core1 waits until
// it is done before touching flash.
#ifndef FLASHSPEED_H
#define FLASHSPEED_H

void flashSpeedApply();       // core0, first thing in setup()
void flashSpeedWaitReady();   // core1, first thing in setup1()

unsigned flashSpeedDiv();   // the divider actually in the register now

// Reads a 512 kB block (past the cache); tens of ms, so only at
// startup, before the producer loop runs.
unsigned flashSpeedMeasureKBs();

unsigned flashSpeedLastKBs();   // last result in kB/s, 0 if not yet run
#endif  // FLASHSPEED_H

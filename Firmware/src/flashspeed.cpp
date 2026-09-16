#include "flashspeed.h"

#include <hardware/regs/addressmap.h>
#include <hardware/structs/qmi.h>
#include <hardware/sync.h>
#include <hardware/timer.h>
#include <pico.h>

#include "config.h"

static volatile bool ready = false;

void __not_in_flash_func(flashSpeedWaitReady)() {
  while (!ready) tight_loop_contents();
}

void __not_in_flash_func(flashSpeedApply)() {
  // 0 means "do not touch" (config.h). W25Q128JV is rated for 133 MHz.
  static_assert(CFG_FLASH_CLKDIV == 0 || (CFG_SYSCLK_HZ / CFG_FLASH_CLKDIV) <= 133000000,
                "CFG_FLASH_CLKDIV gives a QSPI clock above the 133 MHz the flash is rated for");
  if (CFG_FLASH_CLKDIV >= 1) {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t t = qmi_hw->m[0].timing;
    t = (t & ~QMI_M0_TIMING_CLKDIV_BITS) | (CFG_FLASH_CLKDIV << QMI_M0_TIMING_CLKDIV_LSB);
    qmi_hw->m[0].timing = t;
    (void)*(volatile uint32_t *)XIP_BASE;  // first read with the new timing
    restore_interrupts(irq);
  }
  ready = true;
}

unsigned flashSpeedDiv() {
  return (qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
}

static unsigned lastKBs = 0;

unsigned flashSpeedLastKBs() { return lastKBs; }

unsigned flashSpeedMeasureKBs() {
  // 512 kB, past the XIP cache; sum folded into the result so the
  // compiler cannot optimise the loop away.
  static const uint32_t BYTES = 512u * 1024u;
  const volatile uint32_t *p = (const volatile uint32_t *)(XIP_BASE + 0x1000);
  uint32_t t0 = time_us_32();
  uint32_t sum = 0;
  for (uint32_t i = 0; i < BYTES / 4; i++) sum += p[i];
  uint32_t dt = time_us_32() - t0;
  if (dt == 0) return 0;
  lastKBs = (unsigned)(((uint64_t)BYTES * 1000u) / dt) + (sum & 0);
  return lastKBs;
}

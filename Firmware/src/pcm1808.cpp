#include "pcm1808.h"

#include "board.h"
#include "config.h"

#include <Arduino.h>
#include <hardware/dma.h>
#include <hardware/pio.h>

#include "pcm1808.pio.h"

// i2s_rx.pio's "wait gpio 9"/"wait gpio 10" cannot be relocated through
// a pin mapping the way "in pins" can; this keeps them honest if the
// pins ever move.
static_assert(PIN_PCM_BCK == 9, "pcm1808.pio hardcodes GPIO 9 for the BCK wait");
static_assert(PIN_PCM_LRC == 10, "pcm1808.pio hardcodes GPIO 10 for the LRCK wait");

// SCKI: the experiment board has no crystal of its own (see board.h),
// so this generates it. 108 MHz sysclk (CFG_SYSCLK_HZ) has no exact
// divisor for any PCM1808 sample rate -- checked against every row of
// Table 1 in the datasheet -- so this picks the nearest exact AVERAGE:
// 48 kHz, 256 fS = 12.288 MHz, master mode (MD1=High, MD0=High). Two
// PIO instructions toggle the pin once a cycle, so the state machine
// itself must run at 24.576 MHz: 108000000 / 24576000 = 4 + 101/256
// exactly (checked by hand). The fractional divider still jitters
// cycle to cycle, +-1 sysclk period (~9 ns) on a ~40 ns half period,
// unlike CLKIN in video.cpp which uses a sysclk multiple chosen so its
// own divider is a whole number. Real, unmeasured degradation of the
// ADC's own clock reference; acceptable for this experiment, not a
// permanent design.
static const uint16_t PCM_CLKDIV_INT = 4;
static const uint8_t PCM_CLKDIV_FRAC = 101;

static PIO pcmPio = pio1;   // pio0 is bt656_out; this stays off it entirely
static int sckiSm = -1;
static int rxSm = -1;
static int rawDmaChan = -1;

// Raw capture: one 32 bit word per channel, framed by the PIO (see
// pcm1808.pio): bits 31..8 the 24 bit sample, bits 7..1 the ADC's
// padding, bit 0 the channel (0 left, 1 right). 96000 words/s at
// 48 kHz. Polled every line, blanking lines included (the cards'
// vbiLine hook, see pattern.h): measured without that, the stretch
// without a poll across a field's blanking was 2.71 ms, and a 256 word
// ring (2.67 ms) was lapped by a few words every field, 250 words/s
// lost. With a poll every 64 us the backlog never exceeds a dozen
// words, and 256 is ample again; the 512 that bridged the gap for a
// while went back to the RAM budget.
static const int RAW_RING_WORDS = 256;
static const int RAW_RING_BYTES = RAW_RING_WORDS * 4;
static uint32_t rawRing[RAW_RING_WORDS] __attribute__((aligned(RAW_RING_BYTES)));
static uint32_t rawWordsSeen = 0;

// Decoded stereo pairs, 16 bit (the low 8 of the ADC's 24 dropped),
// the small ring PATTERN_PCMAUDIO's renderRow() drains from. A backlog
// (the 2.7 ms blanking gap, the card's text band) waits in the raw
// ring above, not here: a poll moves at most MAX_WORDS_PER_POLL words
// = 16 pairs across while a row takes 4 out, so this fills by 12 a row
// at most and empties again at the net 0.93 a row (MAX_PAIRS 4 out
// against 3.07 in) once the raw ring is caught up. The Sony PCM card
// is the other user: its resampler takes 3.27 a data line and nothing
// during the ~18 lines a field without data, so ~55 pairs pile up
// there and its level servo (asrc.cpp) has to keep that peak below
// this size and the trough above zero. 192 gives it room; 128 with
// the peak held at 64 left a trough of ~9 and audible dropouts.
struct Pair {
  int16_t l, r;
};
// A power of two on purpose: the ring is addressed as
// counter % PAIR_RING_SIZE on free-running uint32 counters, and with
// 192 the mapping jumped 64 slots at the 2^32 wrap (once per ~25 h of
// PCM display), colliding live entries.
static const int PAIR_RING_SIZE = 256;
static Pair pairRing[PAIR_RING_SIZE];
static uint32_t pairWrite = 0;
static uint32_t pairRead = 0;

static int16_t pendingLeft = 0;
static bool haveLeft = false;

static inline void pcmPushPair(bool right, int16_t sample) {
  if (!right) {
    pendingLeft = sample;
    haveLeft = true;
    return;
  }
  if (!haveLeft) return;  // a right with no left before it: drop it
  haveLeft = false;
  if (pairWrite - pairRead >= (uint32_t)PAIR_RING_SIZE) pairRead++;  // full: drop the oldest
  pairRing[pairWrite % PAIR_RING_SIZE] = {pendingLeft, sample};
  pairWrite++;
}

static inline void consumeWord(uint32_t w) {
  // Bits 31..8 hold the sample MSB first; the top 16 are all that is
  // kept, and an arithmetic shift sign-extends for free.
  pcmPushPair((w & 1u) != 0, (int16_t)((int32_t)w >> 16));
}

static void sckiInit() {
  uint offset = pio_add_program(pcmPio, &scki_clock_program);
  sckiSm = pio_claim_unused_sm(pcmPio, true);
  pio_gpio_init(pcmPio, PIN_PCM_SCKI);
  pio_sm_set_consecutive_pindirs(pcmPio, sckiSm, PIN_PCM_SCKI, 1, true);

  pio_sm_config c = scki_clock_program_get_default_config(offset);
  sm_config_set_sideset_pins(&c, PIN_PCM_SCKI);
  sm_config_set_clkdiv_int_frac(&c, PCM_CLKDIV_INT, PCM_CLKDIV_FRAC);

  pio_sm_init(pcmPio, sckiSm, offset, &c);
  pio_sm_set_enabled(pcmPio, sckiSm, true);
}

static void rxInit() {
  // Pulled down, not left floating: with nothing wired up (or the cable
  // unplugged later) these would otherwise pick up noise and toggle on
  // their own. A real PCM1808 driving them as push-pull outputs
  // overrides a weak pulldown without noticing it. With LRCK held low
  // the PIO program simply waits on its first "wait 1 gpio 10" and
  // nothing is ever captured.
  pinMode(PIN_PCM_BCK, INPUT_PULLDOWN);
  gpio_pull_down(PIN_PCM_LRC);
  gpio_pull_down(PIN_PCM_DOUT);

  uint offset = pio_add_program(pcmPio, &i2s_rx_program);
  rxSm = pio_claim_unused_sm(pcmPio, true);
  pio_gpio_init(pcmPio, PIN_PCM_LRC);
  pio_gpio_init(pcmPio, PIN_PCM_DOUT);
  pio_sm_set_consecutive_pindirs(pcmPio, rxSm, PIN_PCM_LRC, 2, false);

  pio_sm_config c = i2s_rx_program_get_default_config(offset);
  sm_config_set_in_pins(&c, PIN_PCM_DOUT);      // "in pins, 1" reads DOUT
  sm_config_set_in_shift(&c, false, true, 32);  // shift left (MSB first), autopush at 32
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  pio_sm_init(pcmPio, rxSm, offset, &c);

  // The DMA is armed before the state machine is enabled, so the first
  // word out of the FIFO can never wait on it.
  rawDmaChan = dma_claim_unused_channel(true);
  dma_channel_config dc = dma_channel_get_default_config(rawDmaChan);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, false);
  channel_config_set_write_increment(&dc, true);
  channel_config_set_dreq(&dc, pio_get_dreq(pcmPio, rxSm, false));  // false: RX
  static_assert(RAW_RING_BYTES == 1024, "ring size below must match RAW_RING_WORDS");
  channel_config_set_ring(&dc, true, 10);  // 2^10 = 1024 bytes = RAW_RING_BYTES

  // ENDLESS mode: RP2350's TRANS_COUNT carries a MODE field in bits
  // 31:28 (hardware/regs/dma.h, DMA_CH0_TRANS_COUNT_MODE_VALUE_ENDLESS
  // = 0xf), in which "the transfer count is not decremented" and the
  // channel runs until aborted. No retrigger and no ceiling, and never
  // any need to restart the channel (see video.cpp's own DMA chain
  // comment on why that is not something to do lightly). It also means
  // TRANS_COUNT is no use as a position reference: pcm1808Poll() reads
  // the write address instead.
  const uint32_t endless = DMA_CH0_TRANS_COUNT_MODE_VALUE_ENDLESS << DMA_CH0_TRANS_COUNT_MODE_LSB;
  dma_channel_configure(rawDmaChan, &dc, rawRing, &pcmPio->rxf[rxSm], endless | 1u, true);

  pio_sm_set_enabled(pcmPio, rxSm, true);
}

static bool adcPresent = false;

void pcm1808Begin() {
  sckiInit();
  rxInit();
  // Whether an ADC answers at all. In master mode the PCM1808 starts
  // BCK/LRCK by itself once SCKI runs, the PIO then pushes and the
  // capture DMA's write address moves (~192k words a second); without
  // an ADC the pull-down on LRCK keeps the PIO waiting forever (see
  // rxInit()). Decided once, here at boot, like the front panel's own
  // PIN_FRONT_PRESENT: the two PCM cards hide on this answer
  // (cardIsAvailable() in Firmware.ino). 50 ms covers the ADC's clock
  // start; with one attached the first movement ends the wait early.
  const uint32_t a0 = (uint32_t)dma_channel_hw_addr(rawDmaChan)->write_addr;
  const uint32_t t0 = time_us_32();
  while ((uint32_t)(time_us_32() - t0) < 50000) {
    if ((uint32_t)dma_channel_hw_addr(rawDmaChan)->write_addr != a0) {
      adcPresent = true;
      break;
    }
  }
}

bool pcm1808Present() { return adcPresent; }

// Forgets everything queued: a card switching to PCM would otherwise
// first play up to a ring of minutes-old pairs.
void pcm1808Flush() {
  pairRead = pairWrite;
  haveLeft = false;
}

// Ceilings on one poll. Real audio needs ~7 words a row; 32 leaves a
// field's blanking backlog (~260 words) cleared in a dozen rows. The
// time cap is belt and braces for the case where noise on floating
// inputs drives the PIO faster than any real ADC would: consuming a
// word is a few dozen cycles, so even 32 of them stay well inside the
// 64 us row budget.
static const uint32_t MAX_WORDS_PER_POLL = 32;
static const uint32_t POLL_BUDGET_US = 16;

// In RAM: see videoBuildLine() on the flash cache.
void __not_in_flash_func(pcm1808Poll)() {
  if (rawDmaChan < 0) return;
  // Position from the write address, modulo the ring: with the channel
  // in ENDLESS mode (see rxInit()) the transfer count never moves, so
  // there is no absolute word count to be had. A reader lapped by the
  // DMA is therefore not detectable and reads a mix of old and new
  // words; the channel flag in bit 0 keeps left and right straight
  // through that regardless. It takes a stretch of more than 2.7 ms
  // without a poll to get there, which only a card change causes.
  uint32_t base = (uint32_t)(uintptr_t)rawRing;
  uint32_t nowAddr = (uint32_t)(uintptr_t)dma_channel_hw_addr(rawDmaChan)->write_addr;
  uint32_t nowWordIdx = (nowAddr - base) / 4;
  uint32_t idx = rawWordsSeen % (uint32_t)RAW_RING_WORDS;
  uint32_t backlog = (nowWordIdx - idx) % (uint32_t)RAW_RING_WORDS;
  if (backlog > MAX_WORDS_PER_POLL) backlog = MAX_WORDS_PER_POLL;

  const uint32_t t0 = time_us_32();
  uint32_t done = 0;
  while (done < backlog && (time_us_32() - t0) < POLL_BUDGET_US) {
    consumeWord(rawRing[(rawWordsSeen + done) % (uint32_t)RAW_RING_WORDS]);
    done++;
  }
  rawWordsSeen += done;
}

int pcm1808Available() {
  return (int)(pairWrite - pairRead);
}

int __not_in_flash_func(pcm1808TakePairs)(int16_t *l, int16_t *r, int max) {
  int n = 0;
  while (n < max && pairRead != pairWrite) {
    const Pair &p = pairRing[pairRead % (uint32_t)PAIR_RING_SIZE];
    l[n] = p.l;
    r[n] = p.r;
    pairRead++;
    n++;
  }
  return n;
}

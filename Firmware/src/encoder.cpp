#include "encoder.h"

#include <Arduino.h>
#include <pico.h>   // __not_in_flash_func

#include "board.h"
#include "config.h"

#if CFG_ENCODER_ON

// Quadrature transition table, indexed by (old code << 2 | new code):
// +1/-1 for a real Gray code step, 0 for no movement or an impossible
// two-step jump (contact bounce).
static const int8_t STEP[16] = {
    0, +1, -1, 0,
    -1, 0, 0, +1,
    +1, 0, 0, -1,
    0, -1, +1, 0,
};

static volatile int16_t steps = 0;

// No time debounce; the discipline is the Gray code itself, a full
// four-quarter commit with a reset at rest.
static uint8_t oldAB = 3;   // rest: both phases pulled up
static int8_t encval = 0;

// Nothing commits until BOOT_GUARD_US after encoderBegin(), so a
// spurious edge while the pull-ups settle at power-on cannot register.
// One-shot on purpose: (int32_t)(now - deadline) >= 0 is a 2^31 us
// window, not a threshold. Left armed, it went false again 35.8
// minutes after boot and the knob fell silent for the next 35.8,
// alternating forever: the ISR kept running and encval kept completing
// clicks while steps stayed 0 (measured with key M, 30 August 2026).
static const uint32_t BOOT_GUARD_US = 300000;
static uint32_t encoderReadyUs = 0;
static bool bootGuardPassed = false;

static inline bool __not_in_flash_func(bootGuardOver)() {
  if (!bootGuardPassed && (int32_t)(time_us_32() - encoderReadyUs) >= 0) bootGuardPassed = true;
  return bootGuardPassed;
}

static void __not_in_flash_func(encoderIsr)() {
  const uint8_t code = (uint8_t)((gpio_get(PIN_ENC_A) << 1) | gpio_get(PIN_ENC_B));
  oldAB = (uint8_t)((oldAB << 2) | code);
  encval = (int8_t)(encval + STEP[oldAB & 0x0f]);

  // Unstick: four straight impossible jumps would otherwise oscillate
  // forever without reaching rest.
  if (!(uint8_t)(255 - oldAB)) encval = 0;

  if (encval > 3) {
    if (bootGuardOver()) steps++;
    encval = 0;
  } else if (encval < -3) {
    if (bootGuardOver()) steps--;
    encval = 0;
  }

  // A partial count that never reached a full click drops once the
  // pins return to rest (code 3, both phases high).
  if ((oldAB & 0x03) == 0x03) encval = 0;
}

// 20 ms of agreement at 64 us a sample, kept in samples so no millis()
// call is needed on the row path.
static const uint16_t PUSH_STABLE = 313;
static uint16_t pushSame = 0;
static bool pushState = true;   // pulled up, so true is not pressed
static volatile bool pushed = false;

// Same debounce as the press: letting go can nudge the shaft too. See
// MENU_TURN_GUARD_MS in Firmware.ino, which arms on this as well.
static volatile bool released = false;

// Fires while still held, not on release, so the menu opens under your
// thumb.
static const uint32_t LONG_PRESS_US = 2000000;
static uint32_t pushDownUs = 0;
static bool longFired = false;
static volatile bool longPress = false;

// Set once encoderBegin() has actually run, so encoderPresent() answers
// "did this board get one" rather than just "was the feature compiled
// in" -- the front panel carrying the knob can be unplugged at runtime
// (see PIN_FRONT_PRESENT in board.h), which CFG_ENCODER_ON cannot see.
static bool began = false;

void encoderBegin() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_PUSH, INPUT_PULLUP);
  oldAB = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  encval = 0;
  steps = 0;
  encoderReadyUs = time_us_32() + BOOT_GUARD_US;
  bootGuardPassed = false;
  pushState = true;
  pushSame = 0;
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encoderIsr, CHANGE);
  began = true;
}

// Rotation commits in encoderIsr(); this only polls the push switch,
// which needs repeated sampling to debounce a level. A no-op before
// encoderBegin() has run: PIN_ENC_PUSH is not yet a configured input,
// so reading it here would be a floating pin, not a real switch.
void __not_in_flash_func(encoderPoll)() {
  if (!began) return;
  // Closes the boot-guard latch on time alone, every 64 us row. Left
  // to the ISR the latch only closed when a click happened to land in
  // the positive half of the 2^31 us window; a first touch of the
  // knob 40+ minutes after boot found it still open with the window
  // negative, and rotation stayed dead again (found in review,
  // 31 August 2026). From here the first row 300 ms after boot
  // closes it for good.
  bootGuardOver();
  const bool now = gpio_get(PIN_ENC_PUSH) != 0;
  if (now == pushState) {
    pushSame = 0;
  } else if (++pushSame >= PUSH_STABLE) {
    pushSame = 0;
    pushState = now;
    if (!now) {   // the press, not the release, so the change is under your thumb
      pushed = true;
      pushDownUs = time_us_32();
      longFired = false;
    } else {
      released = true;
    }
  }
  if (!pushState && !longFired && time_us_32() - pushDownUs >= LONG_PRESS_US) {
    longFired = true;
    longPress = true;
  }
}

int encoderTakeSteps() {
  noInterrupts();   // steps is written from encoderIsr()
  const int16_t n = steps;
  steps = 0;
  interrupts();
  return n;
}

bool encoderTakePush() {
  const bool p = pushed;
  pushed = false;
  return p;
}

bool encoderTakeRelease() {
  const bool r = released;
  released = false;
  return r;
}

bool encoderTakeLongPress() {
  const bool p = longPress;
  longPress = false;
  return p;
}

void encoderFlush() {
  noInterrupts();
  encval = 0;
  oldAB &= 0x03;   // keep the low two bits; encoderIsr() needs them next
  interrupts();
  steps = 0;
}

bool encoderPresent() { return began; }

#else

void encoderBegin() {}
void __not_in_flash_func(encoderPoll)() {}
int encoderTakeSteps() { return 0; }
bool encoderTakePush() { return false; }
bool encoderTakeRelease() { return false; }
bool encoderTakeLongPress() { return false; }
void encoderFlush() {}
bool encoderPresent() { return false; }

#endif

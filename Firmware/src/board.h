// The pins of this board in one place, because several modules use them.
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

static const uint8_t PIN_DATA_BASE = 0;  // GP0..GP7 -> P0..P7 (pixel bus)
static const uint8_t PIN_CLKOUT = 12;    // GP12 -> CLKIN (through JP1, DATA side)
static const uint8_t PIN_HSYNC = 13;     // GP13 -> ~HSYNC, unused; now pio1's PCM1808 SCKI, see below
static const uint8_t PIN_VSYNC = 14;     // GP14 -> ~VSYNC, unused, held high
static const uint8_t PIN_RESET = 15;     // GP15 -> ~RESET
static const uint8_t PIN_SDA = 16;       // GP16 -> SDA  (I2C0)
static const uint8_t PIN_SCL = 17;       // GP17 -> SCL  (I2C0)
static const uint8_t PIN_SFL = 18;       // GP18 -> SFL, idles low, pulse = timing reset

// Status display, ST7789P3 on SPI0.
static const uint8_t PIN_DISP_RST = 19;   // GP19 -> RST
static const uint8_t PIN_DISP_CS = 20;    // GP20 -> CS
static const uint8_t PIN_DISP_DC = 21;    // GP21 -> DC, low = command
static const uint8_t PIN_DISP_SCK = 22;   // GP22 -> SCL   (SPI0 SCK)
static const uint8_t PIN_DISP_MOSI = 23;  // GP23 -> SDA   (SPI0 TX)
static const uint8_t PIN_DISP_BL = 26;    // GP26 -> BL, backlight

// Rotary encoder, internal pull-ups, no external components.
static const uint8_t PIN_ENC_A = 27;      // GP27 -> encoder phase A
static const uint8_t PIN_ENC_B = 28;      // GP28 -> encoder phase B
static const uint8_t PIN_ENC_PUSH = 29;   // GP29 -> the push switch

// GP25 -> activity LED, toggled once a picture in videoFrameHook() so
// it tracks fresh pictures being produced. Not PIN_LED: the
// generic_rp2350 variant's own pins_arduino.h already #defines that
// name to the same pin.
static const uint8_t PIN_ACTIVITY_LED = 25;

// GP24 -> front panel present, on the flatcable to the display/encoder
// PCB: that board ties this pin low, so an internal pull-up reads it
// low when the front panel is fitted and high (nothing pulling it)
// when it is not. Read once in setup(), before displayBegin()/
// encoderBegin(), to skip both when the flatcable is unplugged.
static const uint8_t PIN_FRONT_PRESENT = 24;

// PCM1808 experiment (see notes/hardware.md): GP9-11 were earmarked for
// a microSD that was never built. GP8 stays off limits, it is the real
// PSRAM chip select on the Olimex -XXL (see platformio.ini); GP13
// (PIN_HSYNC) is safe to repurpose only because the encoder is left in
// its reset default with SD sync output disabled (register 0x02) and
// never reads this pin in the SFL/BT.656 timing this firmware uses --
// see advBringUp()/applyOperatingConfig() in adv7391.cpp.
static const uint8_t PIN_PCM_BCK = 9;    // GP9  <- BCK, master mode: output of the ADC
static const uint8_t PIN_PCM_LRC = 10;   // GP10 <- LRCK, idem
static const uint8_t PIN_PCM_DOUT = 11;  // GP11 <- DOUT, idem
static const uint8_t PIN_PCM_SCKI = PIN_HSYNC;  // GP13 -> SCKI, the ADC has no crystal of its own

// BOARD_LABEL and not BOARD_NAME: the Arduino core already defines
// BOARD_NAME on the command line.
static const char BOARD_LABEL[] = "PAL-sign RP2350";

#endif  // BOARD_H

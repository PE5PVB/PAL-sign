// The settings you are most likely to want to change.
//
// The other knobs sit with the code they belong to:
//   pm5644g00.cpp  the positions of the text boxes
//   adv7391.cpp    the luma filter (also live with 1/2/3 over serial)
//   platformio.ini the clock speed
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

// The version on the splash of the little panel.
#define CFG_VERSION "v1.00"

// Station identification. Allowed: A-Z, 0-9, space, the slashed O and
// : ! ? / @ # * _ + < > - . , = ( )   Lower case becomes upper case.
static const char CFG_TEXT_ID[] = "MYCALL";
static const char CFG_TEXT_SUB[] = "MYQTH";

// --- Ticker ----------------------------------------------------------
//
// One line of text scrolling right to left across the picture. Off by
// default. CFG_TICKER_Y is the top row, clamped on screen and rounded
// to an even row (an odd edge flickers between fields). CFG_TICKER_FONT
// indexes FONTS[] in font_data.h. Whether the ticker shows is kept per
// card, see cfgCardTicker() in settings.h.
static const int CFG_TICKER_Y = 530;
static const int CFG_TICKER_FONT = 0;
static const char CFG_TICKER_TEXT[] = "TEST CARD PAL-SIGN";

// Which of the five fixed sizes the big faces (Inter, Doto) draw at;
// see settings.h's TICKER_SIZE_MIN/MAX and ticker.cpp's
// TICKER_SIZE_COUNT. 2 is medium, the one size both faces had before
// this feature existed.
static const int CFG_TICKER_SIZE = 2;

// --- Insert boxes (the ROM test card only) ---------------------------
//
// Middle left and middle right. Part of the EPROM data itself, so
// switching mode switches the picture data too, not just the text.
//
//   0 = no boxes, the grid carries on   (pat2)
//   1 = the time only                   (pat1)
//   2 = date and time                   (pat0)
//   3 = the fixed texts below           (pat0)
//
// Key i walks 0, 1 and 2. Mode 3 is chosen over the port. Each box is
// 148 x 40 px, about 8 characters.
static const int CFG_INSERT_MODE = 2;

// The starting point for mode 3, kept in flash from then on.
#define CFG_ENCODER_ON 1

// Whether the firmware build counts in the card checksum: on, every
// reflash costs the PC all pictures. Off while the firmware is under
// active work.
static const bool CFG_CARD_CRC_BUILD_ID = false;

// How bright the little panel is, 0 to 100.
static const int CFG_DISPLAY_BRIGHTNESS = 100;

static const char CFG_TEXT_INSERT_LEFT[] = "FIXED";
static const char CFG_TEXT_INSERT_RIGHT[] = "TEXT";

// --- The G924 test card only ----------------------------------------
//
// The two narrow chroma test bars left/right, the PAL switch test.
static const bool CFG_G924_CHROMA_BARS = true;

// --- Test Card G only -------------------------------------------------
//
// The two variants differ only in the block at the top left.
// false = AP1, true = AP2.
static const bool CFG_TESTCARDG_AP2 = false;

// --- The slideshow's fade ----------------------------------------------
//
// Off cuts straight to black and back instead of a gradual dip.
static const bool CFG_SLIDE_FADE_ON = true;

// --- The clock's EU summer time ----------------------------------------
static const bool CFG_DST_AUTO_ON = false;

// --- Flash speed ------------------------------------------------------
//
// clk_sys divided by this number. 0 leaves the QMI as the bootrom set
// it up. The W25Q128JV is rated for 133 MHz; flashSpeedApply() carries
// a static_assert on sysclk/divisor so a wrong combination fails the
// build.
static const int CFG_FLASH_CLKDIV = 1;

// THE SYSTEM CLOCK, set by the firmware itself. The PIO program needs
// sysclk / divisor / 2 = 27 MHz exactly, so sysclk must be a multiple
// of 54 MHz; 108 is the highest such multiple within the RP2350's
// 150 MHz rating. See the workbook before raising this.
static const uint32_t CFG_SYSCLK_HZ = 108000000;

// Pixel bus wired mirrored: P7 on the lowest data GPIO through P0 on
// the highest, which lets the PCB route the bus without crossing eight
// tracks. 0 for a straight bus, 1 for a mirrored one; the reversal
// costs no CPU, see bt656_out_rev in bt656.pio. With 1 the sysclk must
// be a multiple of 108 MHz (four state-machine cycles per CLKIN period
// instead of two).
#define CFG_PIXEL_BUS_REVERSED 1

// --- Clock ------------------------------------------------------------
//
// No battery backed RTC: after a power cut the clock starts again here.
// Settable live over serial: T2026-08-09 14:30:00 or T14:30:00.
static const int CFG_CLOCK_START_YEAR = 2026;
static const int CFG_CLOCK_START_MONTH = 1;
static const int CFG_CLOCK_START_DAY = 1;
static const int CFG_CLOCK_START_HOUR = 0;
static const int CFG_CLOCK_START_MIN = 0;
static const int CFG_CLOCK_START_SEC = 0;

// Date format in the left insert box:
//   0 = DD-MM-YY    (09-08-26)
//   1 = DD-MM-YYYY  (09-08-2026)
//   2 = YYYY-MM-DD  (2026-08-09)
static const int CFG_DATE_FORMAT = 0;

// --- Aspect ratio and WSS ---------------------------------------------
//
// WSS (Wide Screen Signalling), line 23. All eight EN 300 294 settings:
//
//   0 = off, no WSS
//   1 = 4:3 full frame
//   2 = 14:9 letterbox, centred
//   3 = 14:9 letterbox, against the top
//   4 = 14:9 full frame
//   5 = 16:9 letterbox, centred
//   6 = 16:9 letterbox, against the top
//   7 = 16:9 anamorphic, only for an already-squeezed source (FUBK 16:9)
//   8 = wider than 16:9, letterbox centred
//
// Switched live with key w.
static const int CFG_ASPECT = 0;

// --- Teletext ---------------------------------------------------------
//
// World System Teletext in lines 7-22 and 320-335; see teletext.h.
// Needs the VBI pass-through of the encoder ON (register 0x83 bit 4,
// switched on at startup; key v turns it off). Switched live with key e.
static const int CFG_TELETEXT = 1;

// Page number in hex the way teletext writes it: 0x100 is page 100 in
// magazine 1. Digits A-F are allowed (ETS 300 706, 9.3.1.1).
static const int CFG_TELETEXT_PAGE = 0x100;

// --- Insertion test signals ------------------------------------------
//
// Four measuring lines per ITU-T J.63; see its.h. Needs VBI open (key
// v), takes lines 17, 18, 330, 331. Switched live with key I.
static const bool CFG_ITS = false;

// --- Contest page -----------------------------------------------------
//
// Black picture, large white text: top line, bottom line, a counter in
// the middle. Colour burst off, mono flag in pattern.h. Factory values;
// all settable over serial and saved, see settings.h.
static const char CFG_CONTEST_TEXT1[] = "MYCALL";
static const char CFG_CONTEST_TEXT2[] = "MYQTH";
static const char CFG_PCM_TEXT[] = "MYCALL";   // the Ham PCM text channel, 10 characters
static const int CFG_CONTEST_NUMBER = 1;

// Maximum character enlargement; the text layer scales itself down if
// it does not fit. Worked out with tools/show_contest.py.
static const int CFG_CONTEST_SCALE_TEXT = 5;
static const int CFG_CONTEST_SCALE_NUMBER = 9;

// --- Serial output ----------------------------------------------------
//
// The timing report every two seconds, off at startup. Switched live
// with key d; ? lists every key.
static const bool CFG_TIMING_REPORT = false;

// --- Test card --------------------------------------------------------
//
// Which test card at startup, index into the PATTERNS table in
// Firmware.ino:
//   0 = PM5644 (G00)    the familiar grid with colour bars
//   1 = PM5644 (G924)   newest generation, near enough the same to look at
//   2 = PM5644 (G913)   monoscope with the Indian head, monochrome
//   3 = FUBK 4:3        the German FUBK
//   4 = FUBK 4:3        the same without the centre circle
//   5 = FUBK 16:9       anamorphic (set CFG_ASPECT to 7)
//   6 = BBC Test Card F
//   7 = BBC Test Card W 16:9 (set CFG_ASPECT to 7)
//   8 = TVE Carta de Ajuste
//   9 = BBC Test Card G
//  10 = colour bars from the encoder itself (diagnostic)
//  11 = pulse and bar   test signal, monochrome
//  12 = sin(x)/x        test signal, monochrome
//  13 = Multiburst      test signal, monochrome
//  14 = EBU black and white, gratings, bar and grey staircase, monochrome
//  15 = EBU colour bars over a red field
//  16 = contest page
//  17..32 = Custom 1 through 16, uploadable (tools/convert_image.py)
//
// Only the choice at startup; keys p and P switch cards live.
static const int CFG_PATTERN = 0;

// --- Status display ---------------------------------------------------
//
// A 2.25 inch 76x284 SPI TFT with an ST7789P3, hung on its side. Shows
// a live miniature of the picture plus status lines. Wiring in board.h.
// Set this to 0 and the module compiles away to empty functions.
#define CFG_DISPLAY_ON 1

// Which SPI block: 0, since SPI1 lands on the microSD pins of this
// board; see PIN_DISP_SCK in board.h.
static const int CFG_DISPLAY_SPI_INDEX = 0;

// The clock on the SPI pins; reported to work up to 80 MHz.
static const uint32_t CFG_DISPLAY_SPI_HZ = 27000000;

// The ST7789 holds 240x320 and this panel is 76x284, so it is a window
// inside that memory: centred, (240-76)/2 = 82 across, (320-284)/2 = 18
// down, swapped on its side. Keys L and l nudge them.
static const int CFG_DISPLAY_COL_START = 18;
static const int CFG_DISPLAY_ROW_START = 82;

// Measured on this panel: RGB, not BGR. Key G tries the other one.
static const bool CFG_DISPLAY_BGR = false;
static const bool CFG_DISPLAY_INVERT = false;

// Which way the backlight pin goes: measured low (BL to ground lights
// it) on this panel. Key B tries the other one.
static const bool CFG_DISPLAY_BL_ACTIVE_HIGH = false;

// How often the miniature is refreshed, in pictures; also the sampling
// rate for the thumbnail rows.
static const int CFG_DISPLAY_THUMB_EVERY = 8;
#endif  // CONFIG_H

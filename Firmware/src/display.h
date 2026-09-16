// The status display: a 2.25 inch 76x284 SPI TFT next to the video out,
// an ST7789P3 hung on its side. Shows a live miniature of the outgoing
// picture and what the serial port would otherwise report.
//
// Never holds up the picture: a full screen write is 43 kB, so the
// transfer is pushed along in half-kilobyte pieces, only while the
// ring buffer has rows to spare.
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

// The picture as we address it, with the panel on its side.
static const int DISP_W = 284;
static const int DISP_H = 76;

// The miniature, on the left, 4:3 at the full panel height.
static const int DISP_THUMB_W = 101;
static const int DISP_THUMB_H = DISP_H;

// Sets up the SPI and the panel, draws the splash, turns the backlight
// on. First call in setup(). No-op when CFG_DISPLAY_ON is false.
void displayBegin();

// Replaces the splash with the working layout, once, at the end of the
// bring-up.
void displaySplashDone();

// Progress bar under the splash, 0..100; no-op after displaySplashDone().
void displayBootProgress(int percent);

// Countdown from secondsLeft to 0 over the splash, for a factory reset
// held at power-on.
void displayBootResetCountdown(int secondsLeft);

// Restores the ordinary splash text after a countdown released before 0.
void displayBootSplashRestore();

// Backlight brightness, 0-100, via PWM; the value itself lives in settings.
void displaySetBrightness(int percent);
int displayBrightness();

bool displayOn();

// Hands one finished picture row to the miniature; base is 4:2:2
// (1440 bytes), y is 0..575. mayWork is false while a card change is
// being copied in.
void displaySampleRow(const uint8_t *base, int y, bool mayWork);

// The two text lines beside the miniature: 0 the card name, 1 the line
// under it. Composed in Firmware.ino; an unchanged line is not resent.
void displaySetLine(int i, const char *text);

// The band under the title: card number left, identification behind
// it. dim greys the identification out, matching one switched off.
void displaySetInfo(const char *left, const char *right, bool dim);

// What the knob is offering, or nullptr for nothing. While set, the
// panel drops the miniature/lines/info band/badges and shows just the
// name, large and centred, with a hint and a count ("3/24"). Composed
// in Firmware.ino.
void displaySetCandidate(const char *name, const char *count);

// A fullscreen status message while the PC tool downloads, erases or
// uploads a card: takes the whole panel like displaySetCandidate(),
// and takes priority over it and the menu. nullptr restores the
// working layout. Composed in Firmware.ino, around the PC commands
// that drive it.
void displaySetBusy(const char *text);

// The on-device menu, mutually exclusive with displaySetCandidate
// (nullptr label restores the working layout). Label small and dim,
// value large below it (or a windowed text field being typed); hint
// left and count right at the bottom. Composed in Firmware.ino.
//
// leftAlign starts the value at the left edge instead of centring.
// cursorIndex is the character under the edit cursor, -1 for none;
// cursorVisible only blinks the bar, independent of the real position.
void displaySetMenu(const char *label, const char *value, const char *hint, const char *count,
                     bool leftAlign, int cursorIndex, bool cursorVisible);

// A bargraph in place of the big centred number, for the menu's
// sliders and the chip's analogue controls: lo/hi/value are the
// control's range and reading. Called every picture alongside
// displaySetMenu(); off, that function's value draws as usual.
void displaySetMenuBar(bool on, int lo, int hi, int value);

// The row of badges along the bottom: a short label in a frame, lit
// when that thing is on. Labels are fixed width so the row never
// changes; an empty label leaves that badge out.
static const int DISPLAY_BADGES = 7;
void displaySetBadge(int i, const char *label, bool on);

// A card the encoder builds itself ignores the pixel bus, so there is
// nothing to sample for the miniature. Such a card holds it and draws
// once from its own init; selectPattern() releases the hold first.
void displayThumbHold(bool hold);
void displayThumbPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);

// Copies one miniature row over another, for a stand-in whose rows
// are all the same.
void displayThumbCopyRow(int to, int from);

// Once per picture, between two frames: refreshes changed text and
// starts a new transfer if the last one finished.
void displayFrameHook();

// Pushes the transfer along by at most `bytes`; returns whether
// anything is left.
bool displayStep(unsigned bytes);
bool displayBusy();

// The ST7789 holds 240x320 and this panel is an undocumented-offset
// window inside that. displayTestCard() draws a frame with a corner
// marker in each corner to find it; keys on the serial port nudge the
// offsets, displayReport() prints them for config.h.
void displayTestCard();
void displayNudge(int dcol, int drow);
void displayFlipOrientation();
void displayToggleBgr();
void displayFlipBacklight();

void displayToggleInvert();
void displayReport(char *buf, unsigned n);
#endif  // DISPLAY_H

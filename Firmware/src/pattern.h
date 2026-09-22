// The interface a test card has to meet.
//
// Adding a new one: a .cpp with these functions, and a line in the
// table in Firmware.ino. If the picture comes from an EPROM dump, see
// rompattern.h first.
#ifndef PATTERN_H
#define PATTERN_H

#include <stdint.h>

// A run of 4:2:2 pixel pairs (each pair is the four bytes Cb,Y,Cr,Y)
// whose chroma gets inverted about 128 on alternate frames; see
// Pattern::phaseBars. first/last are pair indices, 0..359.
struct PhasePairRange {
  uint16_t first;
  uint16_t last;
};

struct Pattern {
  const char *name;

  // Once, before the first row is built: tables, text positions.
  void (*init)();

  // Fills row y (0..575) with 720 pixels in 4:2:2, 1440 bytes from
  // base (4 byte aligned). Budget: 64 us, stay well under it.
  void (*renderRow)(uint8_t *base, int y);

  // Optional (NULL for a still card). Once per picture, between
  // frames: a running clock, anything that must not be half old/new.
  void (*tick)();

  // NOTE: mono and encoderMakesIt are filled in by position in the
  // tables at the end of every pattern file; never insert one between
  // them.

  // true = mono picture, encoder switches chroma/burst off.
  bool mono;

  // true = the encoder makes this picture itself, ignores the pixel bus.
  bool encoderMakesIt;

  // Optional (NULL for a card without text). Light half of init(): text
  // layout only, no romLoad(), no black picture.
  void (*reflow)();

  // Optional. Called for every line outside the active picture (the
  // vertical blanking, line numbers 1..625) with the 1440 byte active
  // part of that line; returns true when it filled it, which takes
  // precedence over ITS and teletext there. A card that carries data
  // in the blanking (Sony PCM: 295 lines a field against the 288 the
  // picture has) draws it here; a card that only needs to be called
  // every line (polling an ADC ring) returns false.
  bool (*vbiLine)(uint8_t *base, uint32_t line);

  // true: keep pixels 0..9 and 712..719, which are otherwise forced to
  // black as BT.601 line blanking. A data card whose bit pattern starts
  // at pixel 2 needs them.
  bool keepEdges;

  // true: the card owns the whole raster. No ticker band or overlay, no
  // aspect-ratio bars or squeeze, no WSS (the card switches it off in
  // its init), and no teletext or ITS in the blanking: a data format
  // that counts lines from the vertical sync cannot have any of them.
  bool rawSignal;

  // Optional (NULL/0 for a still card). A test card stored as two PAL
  // phase frames (PM5644 and kin) alternates at 25 Hz: on every other
  // frame the chroma of these pixel-pair ranges is inverted about 128, a
  // 180 degree subcarrier phase flip of the delay-line reference bars.
  // The luma and the rest of the chroma are identical in both frames, so
  // the inversion runs unconditionally over these ranges: outside the
  // bars the chroma is neutral 128 and 256-128 leaves it untouched.
  const PhasePairRange *phaseBars;
  int phaseBarCount;

  // true when the reference bars already carry the +-V PAL-switch
  // alternation in the pixel data (both PM5644 G00 and G924 do, measured
  // from the ROM): the phase flip is then XORed with the analogue line
  // parity so it survives the encoder's own V-switch. false would suit
  // bars stored as a solid colour, where the encoder makes the alternation
  // and a plain per-frame flip is right.
  bool phaseBarsLineComp;
};

// The pattern the video layer is sending out at the moment.
extern const Pattern *activePattern;

// Cards live in src/testcards/; src/ itself stays infrastructure only.
extern const Pattern PATTERN_PM5644G00;   // PM5644, older generation: grid and colour
extern const Pattern PATTERN_PM5644G913;  // PM5644 G913: monoscope, monochrome
extern const Pattern PATTERN_PM5644G924;  // PM5644 G924: newest generation
extern const Pattern PATTERN_FUBK4X3;     // German FUBK
extern const Pattern PATTERN_FUBK16X9;    // German FUBK, anamorphic 16:9
extern const Pattern PATTERN_FUBKNOCIRCLE;  // FUBK 4:3 without the centre circle
extern const Pattern PATTERN_INTERNBAR;   // colour bars from the encoder itself
extern const Pattern PATTERN_PULSEBAR;    // test signal, pulse and bar
extern const Pattern PATTERN_SINXX;       // test signal, sin(x)/x
extern const Pattern PATTERN_CONTEST;     // black picture, large text and a counter
extern const Pattern PATTERN_TESTCARDF;   // BBC Test Card F, raw out of flash
extern const Pattern PATTERN_TESTCARDG;   // BBC Test Card G
extern const Pattern PATTERN_TESTCARDW;   // BBC Test Card W, 16:9
extern const Pattern PATTERN_TVE;         // TVE Carta de Ajuste, Spain
extern const Pattern PATTERN_MULTIBURST;  // test signal: bursts of frequencies
extern const Pattern PATTERN_CROSSHATCH;  // the PM5644 G00 grid, redrawn as white on black
extern const Pattern PATTERN_EBUBW;       // EBU black and white: gratings, bar, grey staircase
extern const Pattern PATTERN_BARSRED;     // EBU colour bars over a red field
extern const Pattern PATTERN_MIXEDBAR;    // bars, a black text band, bars in black and white
extern const Pattern PATTERN_PICDREAM;    // bars, a text box, a short multiburst, a ticker band
extern const Pattern PATTERN_PCMAUDIO;    // Ham PCM: stereo audio as an NRZ bit stream (HQ, LQ, Voice)
extern const Pattern PATTERN_SONYPCM;     // EIAJ/IEC 60841 PCM (Sony PCM-F1/501ES compatible), 14 or 16 bit

// Which slot an uploadable card belongs to, or -1 for every other card;
// the file name of the picture in it. Both live in custom.cpp.
int customSlotOf(const Pattern *p);
const char *customFileName(const Pattern *p);

extern const Pattern PATTERN_CUSTOM1;
extern const Pattern PATTERN_CUSTOM2;
extern const Pattern PATTERN_CUSTOM3;
extern const Pattern PATTERN_CUSTOM4;
extern const Pattern PATTERN_CUSTOM5;
extern const Pattern PATTERN_CUSTOM6;
extern const Pattern PATTERN_CUSTOM7;
extern const Pattern PATTERN_CUSTOM8;
extern const Pattern PATTERN_CUSTOM9;
extern const Pattern PATTERN_CUSTOM10;
extern const Pattern PATTERN_CUSTOM11;
extern const Pattern PATTERN_CUSTOM12;
extern const Pattern PATTERN_CUSTOM13;
extern const Pattern PATTERN_CUSTOM14;
#endif  // PATTERN_H

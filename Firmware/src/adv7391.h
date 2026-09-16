// ADV7391 video encoder: I2C configuration and bring-up.
//
// Reference: ADV7390/91/92/93 datasheet (Rev. I), Analog Devices.
#ifndef ADV7391_H
#define ADV7391_H

#include <stdint.h>

// Reset, I2C scan, register check, DACs on, PAL + external pixel data.
// Returns false when the encoder does not answer.
bool advBringUp();

// Luma filter, register 0x80: SSAF, LPF PAL, CIF, QCIF, Notch PAL. No
// pass-through setting; every code is a filter.
int advLumaFilterCount();
const char *advLumaFilterName(int idx);
const char *advLumaFilterShortName(int idx);   // for the on-device menu
uint8_t advLumaFilterReg(int idx);
int advLumaFilterIndex();
void advSetLumaFilter(int idx);

// Chroma filter, register 0x80 bits 7:5. Reset default 1.3 MHz.
int advChromaFilterCount();
const char *advChromaFilterName(int idx);
const char *advChromaFilterShortName(int idx);
int advChromaFilterIndex();
void advSetChromaFilter(int idx);

// The encoder's own colour bar generator, register 0x84 bit 6: ignores
// the pixel bus and makes the picture itself.
bool advColourBars();
void advSetColourBars(bool on);

// Chroma and colour burst off, register 0x84.
bool advMonochrome();
void advSetMonochrome(bool on);

// Writes the working registers again, no reset, no self test.
void advReapplyConfig();

// Full hardware reset plus reconfiguration; over 100 ms.
void advHardRestart();

// WSS on picture line 23; the encoder makes the signal, we supply the
// 14 data bits. PAL only.
//
//   0x99 bit 7    WSS on/off
//   0x9A bits 5:0 WSS bits W13..W8
//   0x9B bits 7:0 WSS bits W7..W0
void advSetWss(bool on, uint16_t word);

// VBI open, register 0x83 bit 4: off blanks the whole vertical
// blanking, on lets PAL lines 7-22 out.
bool advVbiOpen();
void advSetVbiOpen(bool on);

// --- fading ------------------------------------------------------------
//
// Scales Y/Cb/Cr output in 256ths, sync untouched (unlike the 0x0B DAC
// gain). Double buffered, so a step lands on the next field boundary.
void advSetFade(int level);
int advFadeLevel();

// Seven analogue controls, each an enable bit in register 0x87 plus its
// data register(s):
//
//   DAC gain      0x0B          +-7.5% of the whole output, sync too
//   Y scale       0x9D + 0x9C   factor 0.0 to 1.5 on the luma
//   brightness    0xA1[6:0]     setup in IRE, PAL -7.5 to +15
//   hue           0xA0          +-22.5 degrees of subcarrier phase
//   luma SSAF     0xA2[3:0]     -4 to +4 dB of mid band tilt
//   luma delay    0x8A[5:4]     0/2/4/6 clocks
//   chroma delay  0x89[5:4]     0/4/8 clocks
//
// Neutral is not always zero (hue 0xA0=0x80, SSAF 0xA2=0x06), so the
// enable bit and its data register are always written together.
int advParamCount();
int advParamIndex();
void advParamSelect(int i);
void advParamStep(int direction);
void advParamNeutral();
void advParamText(char *buf, int len);
void advParamWriteAll();

const char *advParamName(int i);
int advParamNeutralValue(int i);
void advParamLoad(int i, int v);   // sets without writing to the chip

// Same reason as advParamLoad(): settings are read before the encoder
// is up.
void advLoadFilters(int luma, int chroma);
int advParamMin(int i);
int advParamMax(int i);
int advParamStepSize(int i);
int advParamValue(int i);
void advParamSet(int i, int v);

// Timing reset via the SFL pin, register 0x84 bits[2:1] = 10: on
// release the encoder resumes at field 1, subcarrier phase zero. The
// pulse itself comes from videoRequestSflPulse(); call
// advSetTimingReset(true) first.
bool advTimingReset();
void advSetTimingReset(bool on);

// Active field number, register 0xBB bits[2:0], read only.
uint8_t advReadFieldCount();

// Whether a register we wrote is still what we wrote (one register per
// call, from the frame hook; readBack/expected filled in on mismatch).
bool advConfigCheckStep(uint8_t *reg, uint8_t *readBack, uint8_t *expected);

// Closed captioning is not implemented: the chip's generator (registers
// 0x91-0x94, 0x83 bits[6:5]) is NTSC only.

#endif  // ADV7391_H

#include "adv7391.h"

#include "board.h"
#include "its.h"
#include "language.h"
#include "settings.h"
#include "teletext.h"

#include <Arduino.h>
#include <Wire.h>

static const uint8_t RESET_PIN = PIN_RESET;
static const uint32_t ADV_WAKE_MS = 500;   // datasheet gives no wake time; ample margin

static const uint32_t I2C_FREQ = 100000;

// ALSB tied to +3V3 -> 7 bit address 0x2B.
static const uint8_t ADV7391_ADDR = 0x2B;

static const uint8_t REG_POWER = 0x00;         // reset default 0x12
// Scratch register for the read/write self test: inert while its enable
// bit in 0x87 is off (reset state). Do NOT use 0x0B (DAC output level).
static const uint8_t REG_HUE = 0xA0;
static const uint8_t REG_MODE_SELECT = 0x01;   // input mode, reset default 0x00
static const uint8_t REG_SOFT_RESET = 0x17;    // bit 1, self clearing
static const uint8_t REG_SD_MODE1 = 0x80;
static const uint8_t REG_SD_MODE2 = 0x82;
static const uint8_t REG_SD_MODE4 = 0x84;

static void wrMode4();  // 0x84, but only when the byte really changes
static const uint8_t REG_SD_MODE7 = 0x88;
static const uint8_t REG_SD_MODE3 = 0x83;   // among others VBI open (bit 4)
static const uint8_t REG_SD_MODE6 = 0x87;   // the enable bits of the controls; Rev. I numbering
static const uint8_t REG_SD_MODE8 = 0x89;   // among others chroma delay (bits[5:4])
static const uint8_t REG_SD_TIMING0 = 0x8A;  // among others luma delay (bits[5:4])
static const uint8_t REG_DAC_GAIN = 0x0B;
static const uint8_t REG_SCALE_LSB = 0x9C;
static const uint8_t REG_Y_SCALE = 0x9D;
static const uint8_t REG_CB_SCALE = 0x9E;
static const uint8_t REG_CR_SCALE = 0x9F;
static const uint8_t REG_BRIGHTNESS = 0xA1;
static const uint8_t REG_SSAF_GAIN = 0xA2;
static const uint8_t REG_FIELD_COUNT = 0xBB;
static const uint8_t REG_CGMS_WSS0 = 0x99;
static const uint8_t REG_CGMS_WSS1 = 0x9A;
static const uint8_t REG_CGMS_WSS2 = 0x9B;
static const uint8_t REG_FSC0 = 0x8C;
static const uint8_t REG_FSC1 = 0x8D;
static const uint8_t REG_FSC2 = 0x8E;
static const uint8_t REG_FSC3 = 0x8F;

static const uint8_t POWER_RESET_DEFAULT = 0x12;

static void wr(uint8_t reg, uint8_t val);  // defined below
static uint8_t rd(uint8_t reg);            // idem

// Register 0x80 = chroma[7:5] | luma[4:2] | standard[1:0]; standard
// stays 01 (PAL B/D/G/H/I). No pass-through setting exists.
struct Filter {
  uint8_t code;
  const char *name;       // full reading, PC tool + [filter]/[chroma] lines
  const char *shortName;  // for the on-device panel (DISP_W is too narrow)
};

// PAL settings only; the NTSC variants (000 and 010) are skipped.
static const Filter LUMA_FILTERS[] = {
    {0x4, LANG_FILTER_SSAF_FULL, LANG_FILTER_SSAF_SHORT},
    {0x1, LANG_FILTER_LPF_PAL_FULL, LANG_FILTER_LPF_PAL_SHORT},
    {0x3, LANG_FILTER_NOTCH_PAL_FULL, LANG_FILTER_NOTCH_PAL_SHORT},
    {0x5, LANG_FILTER_LUMA_CIF_FULL, LANG_FILTER_CIF_SHORT},
    {0x6, LANG_FILTER_LUMA_QCIF_FULL, LANG_FILTER_QCIF_SHORT},
};
static const int LUMA_FILTER_COUNT = (int)(sizeof(LUMA_FILTERS) / sizeof(LUMA_FILTERS[0]));
static const int LUMA_SSAF = 0;   // the only one wide enough for teletext and ITS
static int lumaFilterIdx = 0;

// 1.3 MHz is the PAL value and the reset default.
static const Filter CHROMA_FILTERS[] = {
    {0x0, LANG_FILTER_1_3MHZ_FULL, LANG_FILTER_1_3MHZ_SHORT},
    {0x1, LANG_FILTER_0_65MHZ_FULL, LANG_FILTER_0_65MHZ_SHORT},
    {0x2, LANG_FILTER_1_0MHZ_FULL, LANG_FILTER_1_0MHZ_SHORT},
    {0x3, LANG_FILTER_2_0MHZ_FULL, LANG_FILTER_2_0MHZ_SHORT},
    {0x7, LANG_FILTER_3_0MHZ_FULL, LANG_FILTER_3_0MHZ_SHORT},
    {0x5, LANG_FILTER_CIF_SHORT, LANG_FILTER_CIF_SHORT},
    {0x6, LANG_FILTER_QCIF_SHORT, LANG_FILTER_QCIF_SHORT},
};
static const int CHROMA_FILTER_COUNT = (int)(sizeof(CHROMA_FILTERS) / sizeof(CHROMA_FILTERS[0]));
static int chromaFilterIdx = 0;

static bool monochroom = false;

// Shadow byte for 0x84: also carries the colour bar generator (bit 6),
// active line length (bit 3) and SFL/reset mode (bits 2:1).
static uint8_t mode4Shadow = 0x00;

static uint8_t mode4Reg() {
  // bit 4 = chroma off, bit 5 = burst off (table 24)
  return (uint8_t)((mode4Shadow & ~0x30u) | (monochroom ? 0x30u : 0x00u));
}

// Register 0x84 bits[2:1]: 00 off, 01 subcarrier reset, 10 timing reset,
// 11 SFL lock (needs an external SFL source, which we have none of).
// Mode 10 resets the H/V counters while SFL is high; on release they
// resume at field 1, subcarrier phase zero, fixing the PAL eight field
// sequence.
bool advTimingReset() { return (mode4Shadow & 0x06u) == 0x04u; }

void advSetTimingReset(bool on) {
  mode4Shadow = (uint8_t)((mode4Shadow & ~0x06u) | (on ? 0x04u : 0x00u));
  wrMode4();
}

// Register 0xBB bits[2:0], read only; bits[5:3] reserved, bits[7:6] the
// revision code.
uint8_t advReadFieldCount() { return (uint8_t)(rd(REG_FIELD_COUNT) & 0x07u); }

// Shadow bytes for registers that carry several things at once, seeded
// with their RESET values, not zero.
static uint8_t mode3Shadow = 0x04;    // 0x83, bits[3:2]=01 is 700 mV p-p PrPb
static uint8_t mode6Shadow = 0x00;    // 0x87, every control off
static uint8_t mode8Shadow = 0x00;    // 0x89
static uint8_t timing0Shadow = 0x08;  // 0x8A, bit 3 reserved must-be-1 + slave/timing mode 0
static uint8_t scaleLsbShadow = 0x00;  // 0x9C, reset value

// VBI open, register 0x83 bit 4: off (reset) blanks the whole vertical
// blanking, on lets our VBI data out on PAL lines 7-22.
bool advVbiOpen() { return (mode3Shadow & 0x10u) != 0; }

void advSetVbiOpen(bool on) {
  mode3Shadow = (uint8_t)((mode3Shadow & ~0x10u) | (on ? 0x10u : 0x00u));
  wr(REG_SD_MODE3, mode3Shadow);
}

// Neutral values are NOT all zero: hue 0xA0=0x80 is 0 degrees, SSAF
// 0xA2=0x06 is 0 dB (reset 0x00 is -4 dB), Y scale 512 is factor 1.0.
// DAC gain's percentage is linear over +-0x40=+-7.5%; table 45
// contradicts itself around 0x3E-0x40, so calibrate against a
// measurement. Delay values are our own ns conversion at 27 MHz.
struct ParamDef {
  const char *name;
  int16_t min, max, neutral, step;
};

// Sentence case: these names go out over the "[control]" serial line to
// the PC tool as well as onto the on-device menu.
static const ParamDef PARAMS[] = {
    {LANG_PARAM_DAC_GAIN, -64, 64, 0, 1},        // 0x0B, +-0x40 = +-7.5%
    {LANG_PARAM_Y_SCALE, 256, 768, 512, 8},      // 0x9D/0x9C, value = factor x 512
    {LANG_PARAM_BRIGHTNESS, -15, 30, 0, 1},      // 0xA1, half IRE steps
    {LANG_PARAM_HUE, -127, 127, 0, 1},           // 0xA0, 0.17578125 deg/step, register = 0x80+w
    {LANG_PARAM_LUMA_SSAF, 0, 12, 6, 1},         // 0xA2, 0 = -4 dB, 12 = +4 dB
    {LANG_PARAM_LUMA_DELAY, 0, 3, 0, 1},         // 0x8A bits[5:4], 0/2/4/6 clocks
    {LANG_PARAM_CHROMA_DELAY, 0, 2, 0, 1},       // 0x89 bits[5:4], 0/4/8 clocks
};
static const int PARAM_COUNT = (int)(sizeof(PARAMS) / sizeof(PARAMS[0]));
static int16_t paramValue[PARAM_COUNT];
static bool paramFilled = false;
static int paramChoice = 0;

int advParamMin(int i) { return (i >= 0 && i < PARAM_COUNT) ? PARAMS[i].min : 0; }
int advParamMax(int i) { return (i >= 0 && i < PARAM_COUNT) ? PARAMS[i].max : 0; }
int advParamNeutralValue(int i) { return (i >= 0 && i < PARAM_COUNT) ? PARAMS[i].neutral : 0; }
int advParamStepSize(int i) { return (i >= 0 && i < PARAM_COUNT) ? PARAMS[i].step : 1; }
const char *advParamName(int i) { return (i >= 0 && i < PARAM_COUNT) ? PARAMS[i].name : ""; }

static void paramFill() {
  if (paramFilled) return;
  for (int i = 0; i < PARAM_COUNT; i++) paramValue[i] = PARAMS[i].neutral;
  paramFilled = true;
}

// Fade factor in 256ths, ridden on the same three scale registers as the
// manual Y scale control (see writeScales()), so it multiplies into
// whatever was set by hand; 256 restores that setting exactly.
static int fadeLevel = 256;

// At its neutral value a parameter's enable bit stays off.
static uint8_t mode6Reg() {
  paramFill();
  uint8_t v = mode6Shadow & ~0x1Fu;
  // Bit 0 switches the scale registers on, bit 1 is a limiter; Rev. I
  // recommends enabling both together, so they always are here.
  if (fadeLevel != 256) v |= 0x03u;
  if (paramValue[1] != PARAMS[1].neutral) v |= 0x03u;  // scale on, with its limiter
  if (paramValue[3] != PARAMS[3].neutral) v |= 0x04u;  // bit 2 hue
  if (paramValue[2] != PARAMS[2].neutral) v |= 0x08u;  // bit 3 brightness
  if (paramValue[4] != PARAMS[4].neutral) v |= 0x10u;  // bit 4 luma SSAF
  return v;
}

// The three SD scale registers (0x9C-0x9F): scale value = factor x 512,
// Y 0.0-1.5, Cb/Cr 0.0-2.0. Top 8 bits in 0x9D/0x9E/0x9F, bottom two of
// each in 0x9C (Y[1:0], Cb[3:2], Cr[5:4]); 0x9C[7:6] is the subcarrier
// phase, hence read+mask.
static void writeScales() {
  paramFill();
  int y = (int)paramValue[1] * fadeLevel / 256;
  int c = 512 * fadeLevel / 256;
  if (y > 1023) y = 1023;
  if (c > 1023) c = 1023;

  wr(REG_Y_SCALE, (uint8_t)((y >> 2) & 0xFF));
  wr(REG_CB_SCALE, (uint8_t)((c >> 2) & 0xFF));
  wr(REG_CR_SCALE, (uint8_t)((c >> 2) & 0xFF));

  // Shadow, not read back: safe since bits[7:6] (subcarrier phase) are
  // never written here and stay at reset value 0.
  scaleLsbShadow = (uint8_t)(scaleLsbShadow & ~0x3Fu);
  scaleLsbShadow |= (uint8_t)(y & 0x03);
  scaleLsbShadow |= (uint8_t)((c & 0x03) << 2);
  scaleLsbShadow |= (uint8_t)((c & 0x03) << 4);
  wr(REG_SCALE_LSB, scaleLsbShadow);
}

static void paramWrite(int i) {
  paramFill();
  int16_t w = paramValue[i];
  switch (i) {
    case 0:
      wr(REG_DAC_GAIN, (uint8_t)(w & 0xFF));   // two's complement in a byte
      break;
    case 1:
      writeScales();   // fade multiplies into the same registers
      break;
    case 2: {
      // 7 bit two's complement, IRE x 2.015631; we count in half IRE.
      int ire2 = (int)lroundf(w * 2.015631f / 2.0f);
      if (ire2 > 63) ire2 = 63;
      if (ire2 < -64) ire2 = -64;
      uint8_t v = (uint8_t)(ire2 & 0x7F);
      // Bit 7 is SD Blank WSS Data and must stay zero, or WSS disappears.
      wr(REG_BRIGHTNESS, v);
      break;
    }
    case 3:
      wr(REG_HUE, (uint8_t)(0x80 + w));  // 0x80 is zero degrees
      break;
    case 4:
      wr(REG_SSAF_GAIN, (uint8_t)(w & 0x0F));
      break;
    case 5:
      timing0Shadow = (uint8_t)((timing0Shadow & ~0x30u) | ((w & 3) << 4));
      wr(REG_SD_TIMING0, timing0Shadow);
      break;
    case 6:
      mode8Shadow = (uint8_t)((mode8Shadow & ~0x30u) | ((w & 3) << 4));
      wr(REG_SD_MODE8, mode8Shadow);
      break;
    default:
      break;
  }
  wr(REG_SD_MODE6, mode6Reg());
}

// Fades the whole SD picture, luma and chroma together. 256 normal, 0
// black. Sync is untouched: the scale registers only affect Y/Cb/Cr
// output levels, unlike the DAC gain in 0x0B which moves sync too. The
// three scale registers are double buffered (0x88 bit 2), so a write
// lands on the next field boundary.
void advSetFade(int level) {
  if (level < 0) level = 0;
  if (level > 256) level = 256;
  fadeLevel = level;
  writeScales();
  wr(REG_SD_MODE6, mode6Reg());
}

int advFadeLevel() { return fadeLevel; }

int advParamCount() { return PARAM_COUNT; }
int advParamIndex() { return paramChoice; }

void advParamSelect(int i) {
  if (i >= 0 && i < PARAM_COUNT) paramChoice = i;
}

int advParamValue(int i) {
  paramFill();
  return (i >= 0 && i < PARAM_COUNT) ? paramValue[i] : 0;
}

void advParamSet(int i, int v) {
  advParamLoad(i, v);
  if (i >= 0 && i < PARAM_COUNT) {
    paramWrite(i);
    cfgTouch();
  }
}

// Same as advParamSet() without touching the chip: settings loaded from
// flash before the encoder is up.
void advParamLoad(int i, int v) {
  paramFill();
  if (i < 0 || i >= PARAM_COUNT) return;
  const ParamDef *d = &PARAMS[i];
  if (v < d->min) v = d->min;
  if (v > d->max) v = d->max;
  paramValue[i] = (int16_t)v;
}

void advParamStep(int direction) {
  paramFill();
  const ParamDef *d = &PARAMS[paramChoice];
  int16_t n = (int16_t)(paramValue[paramChoice] + direction * d->step);
  if (n < d->min) n = d->min;
  if (n > d->max) n = d->max;
  paramValue[paramChoice] = n;
  paramWrite(paramChoice);
  cfgTouch();
}

void advParamNeutral() {
  paramFill();
  paramValue[paramChoice] = PARAMS[paramChoice].neutral;
  paramWrite(paramChoice);
  cfgTouch();
}

void advParamText(char *buf, int len) {
  paramFill();
  // Clamp so the compiler can prove the PARAMS index is in range.
  int i = paramChoice;
  if (i < 0 || i >= PARAM_COUNT) i = 0;
  int16_t w = paramValue[i];
  const char *mark = (w == PARAMS[i].neutral) ? LANG_ADV_NEUTRAL_CONTROL_OFF : "";
  switch (i) {
    case 0:
      snprintf(buf, (size_t)len, LANG_S_2F_OF_FULL, PARAMS[i].name,
               w * 7.5 / 64.0, mark);
      break;
    case 1:
      snprintf(buf, (size_t)len, "%s x%.3f%s", PARAMS[i].name, w / 512.0, mark);
      break;
    case 2:
      snprintf(buf, (size_t)len, LANG_S_1F_IRE_S, PARAMS[i].name, w / 2.0, mark);
      break;
    case 3:
      snprintf(buf, (size_t)len, LANG_S_2F_DEGREES_S, PARAMS[i].name, w * 0.17578125, mark);
      break;
    case 4:
      snprintf(buf, (size_t)len, LANG_S_2F_DB_S, PARAMS[i].name, (w - 6) * 8.0 / 12.0, mark);
      break;
    case 5:
      snprintf(buf, (size_t)len, LANG_S_D_CLOCKS_D, PARAMS[i].name, w * 2, w * 2 * 37,
               mark);
      break;
    case 6:
      snprintf(buf, (size_t)len, LANG_S_D_CLOCKS_D, PARAMS[i].name, w * 4, w * 4 * 37,
               mark);
      break;
    default:
      snprintf(buf, (size_t)len, "%s %d", PARAMS[i].name, w);
      break;
  }
}

void advParamWriteAll() {
  paramFill();
  for (int i = 0; i < PARAM_COUNT; i++) paramWrite(i);
}

// Register 0x84 bit 6: the encoder makes the bars itself, ignoring the
// pixel bus. PAL keeps 0xC3 in 0x82; only bit 6 of 0x84 changes (the
// datasheet's 0xCB example is the NTSC pedestal variant).
bool advColourBars() { return (mode4Shadow & 0x40u) != 0; }

void advSetColourBars(bool on) {
  mode4Shadow = (uint8_t)((mode4Shadow & ~0x40u) | (on ? 0x40u : 0x00u));
  wrMode4();
}

static uint8_t mode1Reg() {
  return (uint8_t)((CHROMA_FILTERS[chromaFilterIdx].code << 5) |
                   (LUMA_FILTERS[lumaFilterIdx].code << 2) | 0x01);
}

int advLumaFilterCount() { return LUMA_FILTER_COUNT; }
const char *advLumaFilterName(int idx) { return LUMA_FILTERS[idx].name; }
const char *advLumaFilterShortName(int idx) { return LUMA_FILTERS[idx].shortName; }
uint8_t advLumaFilterReg(int idx) { return LUMA_FILTERS[idx].code; }
int advLumaFilterIndex() { return lumaFilterIdx; }

int advChromaFilterCount() { return CHROMA_FILTER_COUNT; }
const char *advChromaFilterName(int idx) { return CHROMA_FILTERS[idx].name; }
const char *advChromaFilterShortName(int idx) { return CHROMA_FILTERS[idx].shortName; }
int advChromaFilterIndex() { return chromaFilterIdx; }

void advSetChromaFilter(int idx) {
  if (idx < 0 || idx >= CHROMA_FILTER_COUNT) return;
  if (idx == chromaFilterIdx) return;
  chromaFilterIdx = idx;
  wr(REG_SD_MODE1, mode1Reg());
  cfgTouch();
}

bool advMonochrome() { return monochroom; }

void advSetMonochrome(bool on) {
  monochroom = on;
  wrMode4();
}

static void wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ADV7391_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t rd(uint8_t reg) {
  Wire.beginTransmission(ADV7391_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);  // repeated start
  Wire.requestFrom((int)ADV7391_ADDR, 1);
  return Wire.available() ? Wire.read() : 0xFF;
}

// Teletext/ITS need a wide filter; of the five luma filters only SSAF
// (LUMA_SSAF) is wide enough (the PAL notch sits at 4.4 MHz, right on
// the teletext band).
void advSetLumaFilter(int idx) {
  if (idx < 0 || idx >= LUMA_FILTER_COUNT) return;
  if (idx == lumaFilterIdx) return;
  if (idx != LUMA_SSAF && (teletextOn() || itsEnabled())) {
    Serial.print(LANG_ADV_NOTE);
    Serial.print(LUMA_FILTERS[idx].name);
    Serial.println(LANG_IS_TOO_NARROW_FOR);
  }
  lumaFilterIdx = idx;
  wr(REG_SD_MODE1, mode1Reg());
  cfgTouch();
}

// Same as the setters above without touching the chip, for settings
// loaded from flash before the encoder is brought up.
void advLoadFilters(int luma, int chroma) {
  if (luma >= 0 && luma < LUMA_FILTER_COUNT) lumaFilterIdx = luma;
  if (chroma >= 0 && chroma < CHROMA_FILTER_COUNT) chromaFilterIdx = chroma;
}

static bool wssOn = false;
static uint16_t wssWord = 0;

// A repeated write is not free: it costs a chunk of the 2.3 ms a card
// change has in hand.
static int16_t mode4Written = -1;

static void wrMode4() {
  const uint8_t v = mode4Reg();
  if ((int16_t)v == mode4Written) return;
  mode4Written = v;
  wr(REG_SD_MODE4, v);
}

static int32_t wssWritten = -1;

static void writeWss() {
  const int32_t want = (int32_t)wssWord | (wssOn ? 0x10000 : 0);
  if (want == wssWritten) return;
  wssWritten = want;
  // Data first, enable after: otherwise an old word could go out for one frame.
  wr(REG_CGMS_WSS2, (uint8_t)(wssWord & 0xFF));
  wr(REG_CGMS_WSS1, (uint8_t)((wssWord >> 8) & 0x3F));
  wr(REG_CGMS_WSS0, wssOn ? 0x80 : 0x00);
}

void advSetWss(bool on, uint16_t word) {
  wssOn = on;
  wssWord = word & 0x3FFF;
  writeWss();
}

static void applyOperatingConfig() {
  // A real chip reset put 0x84 and the WSS registers back to their
  // defaults while the write-skipping caches still held the old
  // values, so the wrMode4()/writeWss() at the end of this restore
  // path skipped their writes: WSS silently gone, chroma/burst back
  // on for a mono card, and encoderConfigWatch() then repaired 0x84
  // every second, visibly.
  mode4Written = -1;
  wssWritten = -1;

  // 0x10: DAC1 + PLL on. The datasheet's own script uses 0x1C (all three
  // DACs, for S-Video); we send composite from DAC1 only.
  wr(REG_POWER, 0x10);

  wr(REG_MODE_SELECT, 0x00);   // SD input, also the reset default

  wr(REG_SD_MODE1, mode1Reg());  // PAL plus the luma/chroma filter
  wrMode4();                     // chroma and colour burst

  wr(REG_SD_MODE2, 0xC3);   // PAL B/G has no pedestal, unlike the NTSC 0xCB example

  // 8 bit YCbCr input plus double buffering (bit 2), which lands scale/
  // brightness/hue changes on the next field instead of mid-picture.
  wr(REG_SD_MODE7, 0x04);

  wr(REG_SD_MODE3, mode3Shadow);  // VBI open and the PrPb levels
  advParamWriteAll();             // neutral controls only write zeroes back

  // 0x8A not written for its own sake: reset default 0x08 already is
  // slave + timing Mode 0; it does go out via timing0Shadow (luma delay).

  // Subcarrier DDS for PAL: 4.43361875 MHz / 27 MHz * 2^32.
  wr(REG_FSC0, 0xCB);
  wr(REG_FSC1, 0x8A);
  wr(REG_FSC2, 0x09);
  wr(REG_FSC3, 0x2A);

  writeWss();  // aspect ratio must stay where it was after a reset
}

void advReapplyConfig() { applyOperatingConfig(); }

// The registers applyOperatingConfig() writes that can also be read
// back. 0x87 and the controls' data registers are deliberately not in
// the list: a reset changes all eleven below regardless, enough to show
// that it happened.
static uint8_t expectedValue(uint8_t reg) {
  switch (reg) {
    case REG_POWER: return 0x10;
    case REG_MODE_SELECT: return 0x00;
    case REG_SD_MODE1: return mode1Reg();
    case REG_SD_MODE2: return 0xC3;
    case REG_SD_MODE3: return mode3Shadow;
    case REG_SD_MODE4: return mode4Reg();
    case REG_SD_MODE7: return 0x04;
    case REG_FSC0: return 0xCB;
    case (uint8_t)(REG_FSC0 + 1): return 0x8A;
    case (uint8_t)(REG_FSC0 + 2): return 0x09;
    default: return 0x2A;  // REG_FSC0 + 3
  }
}

bool advConfigCheckStep(uint8_t *reg, uint8_t *readBack, uint8_t *expected) {
  static const uint8_t LIJST[] = {REG_POWER,    REG_MODE_SELECT, REG_SD_MODE1,
                                  REG_SD_MODE2, REG_SD_MODE3,    REG_SD_MODE4,
                                  REG_SD_MODE7, REG_FSC0,        (uint8_t)(REG_FSC0 + 1),
                                  (uint8_t)(REG_FSC0 + 2), (uint8_t)(REG_FSC0 + 3)};
  static unsigned beurt = 0;

  uint8_t r = LIJST[beurt];
  beurt = (beurt + 1) % (sizeof(LIJST) / sizeof(LIJST[0]));

  uint8_t w = expectedValue(r);
  uint8_t g = rd(r);
  if (g == w) return true;
  if (reg) *reg = r;
  if (readBack) *readBack = g;
  if (expected) *expected = w;
  return false;
}

static void hwReset() {
  pinMode(RESET_PIN, OUTPUT);
  digitalWrite(RESET_PIN, HIGH);
  delay(10);
  digitalWrite(RESET_PIN, LOW);  // ~RESET is active low
  delay(10);
  digitalWrite(RESET_PIN, HIGH);
  delay(10);  // time for the internal reset and the PLL to settle
}

// Measures the I2C lines before anything goes over them, so an empty bus
// scan can say WHY. Pull-ups exist only when the ADV7391 board has
// power and the wire is in place.
//   HIGH / -      bus idle, pull-ups present, the board has power
//   LOW / HIGH    floating: no power on the ADV board, or no wire
//   LOW / LOW     held low: a short, or a chip holding the bus
static void measureI2cLine(const char *name, uint8_t pin) {
  pinMode(pin, INPUT_PULLDOWN);
  delayMicroseconds(200);
  bool extern_pullup = digitalRead(pin);
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(200);
  bool komt_hoog = digitalRead(pin);

  const char *verdict;
  if (extern_pullup) {
    verdict = LANG_ADV_PULLUP_OK;
  } else if (komt_hoog) {
    verdict = LANG_ADV_FLOATING;
  } else {
    verdict = LANG_ADV_HELD_LOW;
  }
  Serial.printf(LANG_S_ON_GP_2U, name, pin, verdict);
}

bool advBringUp() {
  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.begin();
  Wire.setClock(I2C_FREQ);
  // 3 ms bound so a stuck bus does not stall the scan for minutes.
  Wire.setTimeout(3);

  Serial.println(LANG_1_HARDWARE_RESET);
  hwReset();
  Serial.printf(LANG_RESET_PULSE_GIVEN_ON, RESET_PIN);

  Serial.println(LANG_2_MEASURING_THE_I2C_LINES);
  measureI2cLine(LANG_SDA, PIN_SDA);
  measureI2cLine(LANG_SCL, PIN_SCL);
  // End before begin: begin() returns at once if already running and
  // might not put the pads back to I2C after the GPIO measurement above.
  Wire.end();
  Wire.begin();
  Wire.setClock(I2C_FREQ);
  Wire.setTimeout(3);

  // The encoder can be slow to wake on a cold start; not seen after a
  // UF2 flash, since it is already awake by then.
  Serial.println(LANG_3_I2C_BUS_SCAN);
  bool found = false, advFound = false;
  {
    const uint32_t t0 = millis();
    while (millis() - t0 < ADV_WAKE_MS) {
      Wire.beginTransmission(ADV7391_ADDR);
      if (Wire.endTransmission() == 0) {
        advFound = found = true;
        break;
      }
      delay(2);   // yields, so the USB keeps being served meanwhile
    }
    if (advFound) {
      Serial.printf(LANG_FOUND_0X_02X_AFTER, ADV7391_ADDR,
                    (unsigned long)(millis() - t0));
    }
  }

  if (!advFound) {
    Serial.printf(LANG_0X_02X_DID_NOT,
                  ADV7391_ADDR, (int)ADV_WAKE_MS);
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        Serial.printf(LANG_FOUND_0X_02X, addr);
        found = true;
      }
    }
  }
  if (!found) {
    Serial.println(LANG_NO_I2C_DEVICES_FOUND);
    Serial.println(LANG_CHECK_SDA_SCL_THE);
    return false;
  }
  if (!advFound) {
    Serial.printf(LANG_ERROR_0X_02X_NOT, ADV7391_ADDR);
    Serial.println(LANG_CHECK_THE_ALSB_STRAP);
    return false;
  }
  Serial.printf(LANG_OK_ADV7391_FOUND_AT, ADV7391_ADDR);

  Serial.println(LANG_4_CHECKING_THE_RESET_VALUE);
  uint8_t val = rd(REG_POWER);
  Serial.printf(LANG_READ_0X_02X_EXPECTED, val, POWER_RESET_DEFAULT);
  Serial.println(val == POWER_RESET_DEFAULT
                     ? LANG_ADV_RESET_OK
                     : LANG_ADV_RESET_WARNING);

  Serial.println(LANG_5_READ_WRITE_TEST_ON);
  const uint8_t testPattern = 0x5A;
  wr(REG_HUE, testPattern);
  uint8_t readback = rd(REG_HUE);
  Serial.printf(LANG_WRITTEN_0X_02X_READ, testPattern, readback);
  Serial.println(readback == testPattern ? LANG_ADV_RW_OK : LANG_ADV_RW_ERROR);
  wr(REG_HUE, 0x00);

  Serial.println(LANG_6_TESTING_THE_SOFTWARE_RESET);
  wr(REG_SOFT_RESET, 0x02);
  delay(10);
  val = rd(REG_SOFT_RESET);
  Serial.printf(LANG_0X17_AFTER_THE_SOFTWARE, val);
  Serial.println((val & 0x02) == 0 ? LANG_ADV_SOFTRESET_OK
                                   : LANG_ADV_SOFTRESET_WARNING);

  Serial.println(LANG_7_SWITCHING_ON_DAC1_AND);
  wr(REG_POWER, 0x10);
  delay(50);  // datasheet gives no PLL lock time; caution, not a requirement

  Serial.println(LANG_8_PAL_AND_EXTERNAL_PIXEL);
  applyOperatingConfig();
  Serial.println(LANG_DONE_CVBS_ON_J2);
  return true;
}

void advHardRestart() {
  hwReset();
  applyOperatingConfig();
}

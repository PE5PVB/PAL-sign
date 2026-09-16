"""
Draws the EBU black and white test card.

    python tools/make_ebubw.py tools/EBU_BW_Test_Card.jpg --png compare.png

Delivers everything the test card needs:
  src/testcards/ebubw_{pal,idx,len,start,line}.bin + _data.h + _data.S

The reference is ONLY needed for the comparison picture. Nothing is
taken from it: the whole card is computed. It sits as
tools/EBU_BW_Test_Card.jpg (699x575) next to this script, so that every
measurement below stays checkable. Just like Tcfm.jpg and the other
reference pictures in this directory it is not in git: only the
measurements that come out of it are below, along with where they come
from.

WHY DRAW IT. See the head of make_testcardf.py: taking it over
unprocessed gives the JPEG approximation of the source, areas that
wobble, edges that ramp over three pixels, and measured values that are
wrong. On this card that shows up all the more: the five gratings are
the test card here, and in the reference their amplitude sags from 118
to 44 over the five bands. Drawn, all five stand at the same amplitude,
and then you measure the roll off of the CHAIN instead of that of the
source.

WHAT IS ON THE CARD, from top to bottom:
  - five horizontal bands with vertical sine gratings, each finer than
    the one before
  - a black bar with a white block left and right and a thin white
    vertical line left of the centre
  - at the bottom black on the left, on the right a grey staircase of
    ten steps

THE MEASURED VALUES, and where they come from:

  gratings    0.8 / 1.8 / 2.8 / 3.8 / 4.8 MHz. PUBLISHED: that is the
              series of definition lines of the Philips PM5540, which
              this card comes from, see
              en.wikipedia.org/wiki/Philips_PM5540
              ("gratings corresponding to 0.8, 1.8, 2.8, 3.8 and 4.8 MHz").

              The reference confirms that series.

              All five frequencies are far below the 6.75 MHz that is
              still possible at 13.5 MHz sampling (Nyquist) and below
              the 5.5 MHz where the luminance band of PAL stops. The
              highest has 13.5/4.8 = 2.81 samples per period; the raster
              can carry that.

  amplitude   All five at 127.5 +/- 127.5, so exactly from black to peak
              white. That roll off is that of the SOURCE and not of the
              card; it does not belong in it.

  staircase   Ten grey steps. NO specification is published for this
              (the PM5540 description mentions only the five step
              staircase of the main pattern), so these values come from
              the reference itself, averaged over rows 450..550 and over
              the flat core of every step.

THE GEOMETRY was measured in the 699x575 reference, and then converted
to our raster: x times 720/699 = 1.03004 and y times 576/575 = 1.00174.
Behind every measurement below stands where it comes from.

ANAMORPHIC does not play a part here: everything on this card is
straight. The only measurement that would depend on the aspect ratio is
the thickness of the white line relative to its surroundings, and that
was measured in pixels and put back down in pixels.

INTERLACE. Every horizontal band boundary lies on an EVEN picture row,
so that every band has an even number of rows. See even_band() in
make_testcardf.py for why: even picture rows go to field 1 and odd ones
to field 2, so a band with an odd number of rows comes out one row
thicker in the one field than in the other and flickers at 25 Hz. The
vertical gratings and the white line do not suffer from that.
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_rom_pattern import encode_and_write
from make_testcardf import W, H, WHITE, BLACK, to_422, fill

FS = 13.5e6

# --- vertical layout ---------------------------------------------------
# Band boundaries measured in the reference: per picture row the
# dominant period from an FFT over columns 30..670. That jumps at row
# 47, 107, 167, 227 and 287; the bar stops at row 407. Times 576/575
# gives 47.08 108.19 167.29 227.40 287.50 and 407.71, and rounded to the
# nearest EVEN row that becomes:
BOUNDARIES = (0, 48, 108, 168, 228, 288, 408, 576)
#             |   |    |    |    |    |    |    +- bottom edge
#             |   |    |    |    |    |    +------ start of staircase part
#             |   |    |    |    |    +----------- start of black bar
#             +---+----+----+----+----------------- the five grating bands
#
# The FIRST band is 48 rows and the other four are 60. That is not a
# rounding error: in the reference the first band runs from row 0 up to
# and including 46 and the other four are 60 rows each. Whether the
# reference is cut off at the top cannot be established, row 0 is half a
# row there, with an average of 50 against 123 for the rest of the band,
# so the measured layout stands.

GRAT_MHZ = (0.8, 1.8, 2.8, 3.8, 4.8)
GRAT_MID = 127.5
GRAT_AMP = 127.5

# --- the black bar -----------------------------------------------------
# Edges measured at half amplitude, averaged over rows 300..395 of the
# reference, and times 720/699:
#   white left runs on to 104.88   -> 108.03
#   white line 204.05 .. 206.58    -> 210.18 .. 212.78 (2.60 wide)
#   white right starts at 603.82   -> 621.96
# The white on the left starts at x 0 and the white on the right runs on
# to x 719: in the reference those are the edges of the picture itself.
#
# The white line becomes 3 px and not 2: 2.60 is closest to that, and
# 210..212 has its centre at 211.5 against 211.48 measured. Vertical
# lines do not flicker, so an odd width is allowed here.
BAR_WHITE_L = (0, 108)
BAR_LINE = (210, 213)
BAR_WHITE_R = (622, 720)

# --- the grey staircase ------------------------------------------------
# The ten transitions on our raster: 162.15 212.21 262.07 311.98 362.31
# 412.51 462.94 512.94 563.26 613.71. The steps are therefore equally
# wide; a straight line through them gives start 161.82 and pitch
# 50.175, and not one measured transition deviates more than 0.37 px
# from that. Hence the regular pitch and not ten separate numbers.
STAIR_X0 = 161.82
STAIR_PITCH = 50.175
# The TENTH step runs on to the right edge instead of stopping after 50
# px: between x 613.7 and the picture edge no transition can be measured
# in the reference. It is peak white as well, 252.6 against 253.4 for
# the white block in the bar above it, and that difference falls within
# the JPEG noise.
#
# The levels, averaged over rows 450..550 and over the flat core of every
# step (the std within a step is 0.5):
#   18.1  42.4  67.2  92.9  120.0  146.8  173.6  200.1  226.2  252.6
# Normalised so that the top step is exactly peak white (times
# 255/252.6, a correction of 1.0 percent) and rounded:
STAIR_N = (18, 43, 68, 94, 121, 148, 175, 202, 228, 255)

# --- the two text boxes ------------------------------------------------
# The black bar is the only area on this card that text can go in. It
# runs from x 108 to 621, with the white line at 210..212 inside it. The
# two boxes sit to the RIGHT of that line, writing over it would make
# the measuring element unreadable, and are centred on the picture
# centre x=360, so that the text stands centred on the screen and not
# centred in the black field (that would come out at x 417 and stand
# visibly off centre).
#
# 216..504 is 288 px wide. That leaves 3 px of black between the white
# line and the left hand side of the box; textPrepare already keeps 3 px
# of margin itself. The bar is 120 rows high, so each box becomes 60,
# ample for both fonts (the PM5544 cell is 30 rows high, the PM8546 24).
TEXT_X0, TEXT_X1 = 216, 504
TEXT1_Y = (288, 348)
TEXT2_Y = (348, 408)


def even_boundaries():
    """Every band boundary on an even row, and so every band even high.

    The same rule as even_band() in make_testcardf.py, but here over a
    row of adjoining bands instead of per separate band.
    """
    for g in BOUNDARIES:
        assert g % 2 == 0, "band boundary %d lies on an odd picture row" % g
    for i in range(len(BOUNDARIES) - 1):
        h = BOUNDARIES[i + 1] - BOUNDARIES[i]
        assert h % 2 == 0, "band %d is %d rows high and that is odd" % (i, h)


def draw_gratings(b):
    """The five sine gratings, over the full picture width.

    The phase starts at zero at x=0, just as in make_testcardf.py. A
    sine and not a square wave: in the reference the second harmonic of
    band 1 is only 3.8 percent of the fundamental and the third 1.2
    percent, and with a square wave that third would sit at 33 percent.
    """
    n = np.arange(W)
    for i, mhz in enumerate(GRAT_MHZ):
        y0, y1 = BOUNDARIES[i], BOUNDARIES[i + 1] - 1
        v = np.clip(GRAT_MID + GRAT_AMP * np.sin(2 * np.pi * mhz * 1e6 * n / FS), 0, 255)
        b[y0:y1 + 1, :] = v[None, :, None]


def draw_bar(b):
    y0, y1 = BOUNDARIES[5], BOUNDARIES[6] - 1
    fill(b, 0, W - 1, y0, y1, BLACK)
    fill(b, BAR_WHITE_L[0], BAR_WHITE_L[1] - 1, y0, y1, WHITE)
    fill(b, BAR_LINE[0], BAR_LINE[1] - 1, y0, y1, WHITE)
    fill(b, BAR_WHITE_R[0], BAR_WHITE_R[1] - 1, y0, y1, WHITE)


def draw_staircase(b):
    y0, y1 = BOUNDARIES[6], BOUNDARIES[7] - 1
    fill(b, 0, W - 1, y0, y1, BLACK)
    for i, level in enumerate(STAIR_N):
        x0 = int(round(STAIR_X0 + i * STAIR_PITCH))
        # The last step runs on to the right edge.
        x1 = (W - 1 if i == len(STAIR_N) - 1
              else int(round(STAIR_X0 + (i + 1) * STAIR_PITCH)) - 1)
        fill(b, x0, x1, y0, y1, (level, level, level))


def draw_card():
    even_boundaries()
    b = np.zeros((H, W, 3), np.float64)
    draw_gratings(b)
    draw_bar(b)
    draw_staircase(b)
    return b


def measure_grating(row, width):
    """Periods over the picture width, from an FFT with a parabolic peak.

    Periods over the width and not MHz straight away: that way how wide
    the picture is in samples does not have to go into it, and with the
    reference that is precisely the unknown. Counting zero crossings is
    not allowed here: on an unsharp source that gives frequencies that
    are too low.
    """
    n = 1 << 15
    s = row - row.mean()
    F = np.abs(np.fft.rfft(s * np.hanning(len(s)), n=n))
    k = int(np.argmax(F[5:])) + 5
    d = 0.5 * (F[k - 1] - F[k + 1]) / (F[k - 1] - 2 * F[k] + F[k + 1])
    return (k + d) / float(n) * width   # (k+d)/n = periods per column


def compare(card, path):
    """What differs from the reference, measured separately per part.

    For the gratings a pixel comparison says NOTHING: two sines that lie
    half a pixel out of phase differ maximally everywhere while it is
    still the same grating. So there the FREQUENCY and the AMPLITUDE are
    compared, and only on the flat parts the difference per pixel.
    """
    im = Image.open(path).convert("L")
    ref_full = np.asarray(im).astype(np.float64)
    ref = np.asarray(im.resize((W, H), Image.LANCZOS)).astype(np.float64)
    print("  compared with %s (%dx%d)" % (os.path.basename(path), im.width, im.height))

    print("    gratings, periods over the picture width (FFT):")
    for i, mhz in enumerate(GRAT_MHZ):
        y0, y1 = BOUNDARIES[i], BOUNDARIES[i + 1]
        yr0 = int(round(y0 * im.height / float(H))) + 3
        yr1 = int(round(y1 * im.height / float(H))) - 3
        pk = measure_grating(card[(y0 + y1) // 2, :, 0], W)
        pr = measure_grating(ref_full[yr0:yr1, 1:im.width - 1].mean(axis=0), im.width)
        sk = card[y0:y1, :, 0]
        sr = ref_full[yr0:yr1, 1:im.width - 1]
        print("      %.1f MHz: drawn %6.2f, reference %6.2f (%+.1f percent); "
              "amplitude drawn %5.1f, reference %5.1f"
              % (mhz, pk, pr, 100.0 * (pr - pk) / pk,
                 (sk.max() - sk.min()) / 2.0, (sr.max() - sr.min()) / 2.0))

    for (i, name) in ((5, "black bar"), (6, "staircase")):
        y0, y1 = BOUNDARIES[i], BOUNDARIES[i + 1]
        d = np.abs(ref[y0:y1] - card[y0:y1, :, 0])
        print("    %-12s row %3d..%3d: on average %.1f grey values difference, "
              "%.2f percent more than 70"
              % (name, y0, y1 - 1, d.mean(), 100.0 * (d > 70).mean()))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("reference", nargs="?",
                   help="only to compare with; nothing is taken from it")
    p.add_argument("--png", help="write the result as a PNG")
    a = p.parse_args()

    card = draw_card()
    print("card drawn, %d different grey values"
          % len(np.unique(np.round(card[..., 0]))))
    print("  bands on row %s" % ", ".join(str(g) for g in BOUNDARIES))
    print("  gratings at %s MHz, all five at %g +/- %g"
          % (", ".join("%g" % m for m in GRAT_MHZ), GRAT_MID, GRAT_AMP))
    print("  staircase: %s" % ", ".join(str(n) for n in STAIR_N))

    if a.png:
        Image.fromarray(np.clip(card, 0, 255).astype(np.uint8)).save(a.png)
        print("  %s written" % a.png)

    if a.reference:
        compare(card, a.reference)

    cfg = {"title": "EBU black and white",
           "note": "fully drawn; gratings 0.8 to 4.8 MHz"}
    encode_and_write("ebubw", cfg, [to_422(card)], 0)


if __name__ == "__main__":
    sys.exit(main())

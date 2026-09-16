#!/usr/bin/env python3
"""Builds the Test Card G tables out of the shipped PM5644 G00 tables.

The card is a derivation from the PM5644 EPROM data that is in the
tree, so the whole card is reproducible from this generator alone.

WHAT MAKES A TEST CARD G, against the PM5544 standard the EPROM card
follows (parameter table in the workbook):

  colour bars      95 percent saturation, 75 percent contrast,
                   25 percent set-up
  block wave       25..100 percent of peak white instead of 0..75
  multiburst       71.4 percent amplitude (0.5 V of 0.7 V, hence 5/7),
                   six sections at 1.5/2.5/3.5/4.0/4.5/5.25 MHz (PAL-I)
                   instead of five at full amplitude

The net effect of the three bar parameters and the block wave is LUMA
PLUS 54.75 (25 percent of 219) with the chroma untouched, byte for
byte, on exactly three areas: the colour bars, the block wave above
them and the bottom segment of the circle. The needle, its white tick,
the grid, the staircase and the side columns stay as the EPROM has
them. The multiburst swings 16..172 around 94, which is 0..71.4
percent.

Deliberately NOT copied from the BBC rendition: its band limited (soft)
edges, and its missing subcarrier test columns. This card keeps the
EPROM's hard edges and every PAL decoder test the PM5644 carries.

Section boundaries of the burst sit one sample left of round numbers,
because the whole bbc frame sits one sample to the right of the EPROM
frame: 205/284/358/437/512.

Every edit below is luma only. The generator checks first that the
chroma is neutral wherever it redraws, and that the base frame holds
the levels this file claims; if the PM5644 tables ever change, it
refuses rather than guessing.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from show_testcard import decode                    # noqa: E402
from convert_rom_pattern import encode_and_write    # noqa: E402

NAME = "testcardg"

SETUP = 219 * 0.25        # 54.75: 25 percent set-up on the changed areas
AMP = 219 * 5.0 / 7.0     # 156.43: the burst swings 0..71.4 percent
FREQS = [1.5, 2.5, 3.5, 4.0, 4.5, 5.25]   # MHz, PAL-I set
FS = 13.5                                  # MHz, the sample rate

# The three +54.75 areas and the burst band, in PM5644 frame coordinates.
BLOCK_Y0, BLOCK_Y1 = 139, 180     # block wave rows, inclusive
BARS_Y0, BARS_Y1 = 181, 266       # colour bar rows, inclusive
BOTTOM_Y0, BOTTOM_Y1 = 470, 545   # bottom segment rows, inclusive
XL, XR = 130, 590                 # nothing outside the circle is touched

BAND_Y0, BAND_Y1 = 307, 390       # burst rows, inclusive
BAND_X0, BAND_X1 = 149, 569       # burst content, inclusive; ramps outside
BOUNDS = [205, 284, 358, 437, 512]

TOL = 4  # a flat level in the EPROM data wobbles a code or three


def luma(frame):
    return frame[:, 1::2]


def classify(row, x, changed, unchanged):
    """-1 unchanged flat, +1 changed flat, 0 transition sample.

    A LEVEL MATCH ALONE IS NOT ENOUGH: the sample must also sit level
    with both neighbours. Without that, the one edge whose slope passes
    through the surround grey gets a sample pinned halfway up: between
    cyan (131) and green (112) the edge crosses 122, one code off the
    grey of 121, and lifting on the value alone leaves exactly that
    sample behind as a fifty-code notch in the flank, visible on
    screen."""
    v = int(row[x])
    if v >= 225:
        return -1   # circle ring and tick white are never lifted
    # A CHANGED level counts on its value alone: where the last white
    # block of the wave narrows to a single sample peak it has no level
    # neighbour, and demanding one left the peak and its flanks behind.
    for c in changed:
        if abs(v - c) <= TOL:
            return 1
    # An UNCHANGED level has to prove itself with at least one level
    # neighbour: the edge between cyan and green passes through 122, one
    # code off the surround grey, and taking that on its value alone
    # pins a fifty code notch halfway up the flank, visible on the
    # screen. The narrow grey pockets in the bottom corners do have a
    # level neighbour, so they rightly stay.
    for u in unchanged:
        if abs(v - u) <= TOL and (abs(int(row[x - 1]) - v) <= 2 or
                                  abs(int(row[x + 1]) - v) <= 2):
            return -1
    return 0


def lift_region(Y, y0, y1, changed, unchanged, stats):
    """Add SETUP to the changed flats; transition samples move along in
    proportion to how far they sit between the changed and the unchanged
    flat beside them, so an edge keeps its shape instead of getting a
    step halfway. Samples between two changed flats move fully, between
    two unchanged flats not at all, and a sample with no flat within
    reach on either side is left alone and counted."""
    for y in range(y0, y1 + 1):
        row = Y[y]
        # Classify AND weigh against an untouched copy: reading the row
        # itself while lifting it made every mixed edge to the right of
        # an already lifted flat come out half weighted.
        #
        # The classification runs EIGHT SAMPLES PAST the window on both
        # sides: an edge just inside the window leans on a flat just
        # outside it, and without the margin those edges were orphans
        # and stayed behind as dips (the right edge of the blue bar
        # against the grey, for one).
        orig = row.copy()
        cls = [classify(orig, x, changed, unchanged) for x in range(XL - 8, XR + 9)]
        for i, x in enumerate(range(XL, XR + 1), start=8):
            c = cls[i]
            if c == -1:
                continue
            if c == 1:
                row[x] = min(235, int(round(orig[x] + SETUP)))
                stats["flat"] += 1
                continue
            # transition: the nearest flat on either side, within reach
            left = right = None
            for j in range(i - 1, max(-1, i - 9), -1):
                if cls[j] != 0:
                    left = j
                    break
            for j in range(i + 1, min(len(cls), i + 9)):
                if cls[j] != 0:
                    right = j
                    break
            if left is None or right is None:
                stats["orphan"] += 1
                continue
            cl, cr = cls[left], cls[right]
            if cl == 1 and cr == 1:
                w = 1.0
            elif cl == -1 and cr == -1:
                w = 0.0
            else:
                a = float(orig[XL - 8 + (left if cl == 1 else right)])   # changed flat
                b = float(orig[XL - 8 + (left if cl == -1 else right)])  # unchanged flat
                w = 0.0 if a == b else (float(orig[x]) - b) / (a - b)
                w = min(1.0, max(0.0, w))
            if w > 0.0:
                row[x] = min(235, int(round(orig[x] + SETUP * w)))
                stats["edge"] += 1


GREY = 121  # the surround grey, for the band's edge ramps


def redraw_burst(Y):
    """Six sections of sine gratings, 0..71.4 percent ON BLACK, exactly
    as the G specification has them. The band's average therefore sits
    below the surround grey and READS AS A DC STEP ON A SCOPE; that is
    the card and not a fault.

    Trough at every section start; the needle block and its white tick
    are left exactly as they are; both edge ramps are redrawn between
    the surround grey and the adjoining sine sample."""
    edges = [BAND_X0] + BOUNDS + [BAND_X1 + 1]
    changed = 0
    for y in range(BAND_Y0, BAND_Y1 + 1):
        row = Y[y]
        # The needle block: solid black around x 350 says this row has
        # it, and then everything between its outer black edges stays.
        block = None
        if int(row[348:356].max()) <= 16 + TOL:
            bl = 348
            while bl > BAND_X0 and row[bl - 1] <= 16 + TOL:
                bl -= 1
            br = 362
            while br < BAND_X1 and row[br + 1] <= 16 + TOL:
                br += 1
            block = (bl, br)
        for s in range(len(FREQS)):
            for x in range(edges[s], edges[s + 1]):
                if block and block[0] <= x <= block[1]:
                    continue
                ph = 2.0 * np.pi * FREQS[s] * (x - edges[s]) / FS
                v = 16.0 + 0.5 * AMP * (1.0 - np.cos(ph))
                row[x] = int(round(v))
                changed += 1
        # the edge ramps, four samples like the EPROM's own edge, from
        # the surround grey to the adjoining sine sample
        vl = float(row[BAND_X0])
        for k, x in enumerate(range(BAND_X0 - 4, BAND_X0)):
            f = (k + 1) / 5.0
            row[x] = int(round(GREY + (vl - GREY) * f))
        v0 = float(row[BAND_X1])
        for k, x in enumerate(range(BAND_X1 + 1, BAND_X1 + 5)):
            f = (k + 1) / 5.0
            row[x] = int(round(v0 + (GREY - v0) * f))
    return changed


def main():
    print("Test Card G, derived from the PM5644 G00 tables")
    base = decode("pm5644g00", 2)   # variant 2: no insert boxes
    frame = base.copy()
    Y = luma(frame)

    # The levels this file claims to find, checked before anything moves.
    checks = [
        (230, 160, 161), (230, 240, 131), (230, 300, 112),   # bars 1..3
        (230, 400, 83), (230, 470, 65), (230, 540, 34),      # bars 4..6
        (160, 200, 16), (160, 300, 180),                     # block wave
        (500, 300, 161), (500, 360, 65),                     # bottom
        (320, 350, 16), (320, 359, 235),                     # needle + tick
    ]
    for y, x, want in checks:
        got = int(Y[y, x])
        if abs(got - want) > TOL:
            raise SystemExit("base frame: expected %d at row %d x %d, found %d"
                             % (want, y, x, got))
    # Chroma must be neutral wherever luma is rewritten. Only the band
    # content itself: the same rows also cross the PAL switch test
    # columns at the outer edge, which are anything but neutral and are
    # not touched here.
    cb = frame[BAND_Y0:BAND_Y1 + 1, 0::4][:, BAND_X0 // 2:BAND_X1 // 2 + 1]
    cr = frame[BAND_Y0:BAND_Y1 + 1, 2::4][:, BAND_X0 // 2:BAND_X1 // 2 + 1]
    if cb.min() < 126 or cb.max() > 130 or cr.min() < 126 or cr.max() > 130:
        raise SystemExit("burst band chroma is not neutral; redrawing luma "
                         "only would tint it")

    stats = {"flat": 0, "edge": 0, "orphan": 0}
    lift_region(Y, BLOCK_Y0, BLOCK_Y1, [16, 180], [121], stats)
    lift_region(Y, BARS_Y0, BARS_Y1, [161, 131, 112, 83, 65, 34], [16, 121], stats)
    lift_region(Y, BOTTOM_Y0, BOTTOM_Y1, [161, 65], [16, 121, 190], stats)
    print("  set-up: %d flat samples lifted 55, %d edge samples in "
          "proportion, %d left alone for lack of a flat neighbour"
          % (stats["flat"], stats["edge"], stats["orphan"]))

    n = redraw_burst(Y)
    print("  multiburst: %d samples redrawn, %s MHz at %.1f percent"
          % (n, "/".join("%g" % f for f in FREQS), 100.0 * AMP / 219))

    # What came out, measured back the way the workbook demands.
    for (x0, x1), f in zip(zip([BAND_X0] + BOUNDS, BOUNDS + [BAND_X1]), FREQS):
        seg = Y[380, x0 + 1:x1 - 1].astype(float)
        zc = np.where(np.diff(np.sign(seg - seg.mean())) != 0)[0]
        per = 2.0 * (zc[-1] - zc[0]) / (len(zc) - 1)
        print("    x %d..%d: %.2f MHz (asked %g), levels %d..%d"
              % (x0, x1, FS / per, f, seg.min(), seg.max()))

    diff = int((frame != base).sum())
    print("  changed: %d of %d bytes (%.2f percent)"
          % (diff, frame.size, 100.0 * diff / frame.size))

    cfg = {"title": "BBC Test Card G",
           "variants": ["PM5644G00_pat2 + Test Card G parameters"],
           "note": "derived from the PM5644 G00 EPROM tables by "
                   "tools/make_testcardg.py: colour bars, block wave and "
                   "bottom segment lifted 25 percent, multiburst redrawn "
                   "at 71.4 percent with the PAL-I frequency set"}
    encode_and_write(NAME, cfg, [frame], 6)


if __name__ == "__main__":
    sys.exit(main())

"""
Builds a second FUBK 4:3 test card: the same picture as PATTERN_FUBK4X3
but without the big centre circle and without the white centre line that
runs through the black text box.

    python tools/make_fubk_nocircle.py

Writes src/testcards/fubknocircle_{pal,idx,len,start,line}.bin plus the
matching _data.h and _data.S, exactly like tools/convert_rom_pattern.py
does for its own sets.

WHERE THE PICTURE COMES FROM

Not from a bitmap. The starting point is the very frame that already
sits in flash for the existing FUBK 4:3: the composite signal
fubk_4x3.bin from hacktv-testsignals, demodulated by tools/paldec.py and
reduced to 64 luma and 32 chroma levels by build_frame_hacktv() in
tools/convert_rom_pattern.py. That call is reused here unchanged, and
main() asserts that the result is byte for byte the same as the shipped
fubk4x3 tables decode to. Everything the circle does not touch is
therefore identical to the card that is already in the firmware.

No ready made source exists for this variant: hacktv-testsignals only
carries the FUBK 4:3 with the circle, and the 768x568 reference bitmap
is a different drawing of the FUBK with a different grid geometry, not
the same picture at another size.

WHAT IS REMOVED, AND WHAT REPLACES IT

The circle is a white ring. Fitted on the 6072 pixels where the picture
differs from the plain grid background outside the centre blocks, with
the centre pinned to the symmetry the picture itself shows (x = 360.0,
because every arc is mirrored about that column, and y = 283.5, the
centre of the horizontal grid lines):

    x0 = 360.0   y0 = 283.5   rx = 245.419   ry = 268.025  (pixels)

That is 6.703 grid cells horizontally and 6.701 vertically, so the ring
is a circle in grid cells rather than on screen. Of those 6072 pixels
none lies further than 2.93 px from the fitted curve, so a mask of 5 px
either side covers the ring including the ringing that the video filter
in the source left around it.

Nothing is painted by hand. Every masked sample is replaced by data that
is already in this same frame:

  * Outside the centre blocks the background is the plain grid. Two rows
    describe it completely, a plain one and one on a horizontal grid
    line, and main() asserts that every unmasked background sample
    already equals that template. Masked samples there are taken from
    the template, chroma from the neutral 128 that the whole background
    carries.

  * Inside the centre blocks a masked run is taken from another row that
    agrees exactly with this row over a window either side of the run.
    All 5148 masked luma samples are covered that way.

  * Chroma inside the blocks cannot always be matched exactly: the
    demodulator leaves a line to line ripple with a period of four
    lines, so 2049 samples come from an exactly matching row and 775
    from the best available one, which differs by at most 3 code values
    over the window. Nothing had to be interpolated. All these counts
    are printed on every run, so a change in the source shows up
    straight away.

The white centre line inside the black text box is the central vertical
grid line running on through the box. It is 5 px wide, rows 285 to 323,
and is set back to the black of the box. The same line above and below
the box stays: the reference keeps it there too.
"""

import os
import sys

import numpy as np

from asmtable import read_table

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from convert_rom_pattern import build_frame_hacktv, encode_and_write

NAME = "fubknocircle"
SRC_FILE = "fubk_4x3.bin"
LUMA_LEVELS, CHROMA_LEVELS, MAX_ERROR = 64, 32, 1   # same as the fubk4x3 set

# Ring, fitted; see the module comment.
X0, Y0, RX, RY = 360.0, 283.5, 245.419, 268.025
MASK_HALF = 5.0

# Grid, measured on the frame: 15 horizontal lines 40 rows apart, the
# first pair on rows 3 and 4.
GRID_ROWS = sorted({3 + 40 * k for k in range(15)} |
                   {4 + 40 * k for k in range(15)})

# Bounding box of the centre blocks, measured: rows 85..484 and columns
# 141..578 differ from the grid background. Widened by 11 columns so the
# filter tails around the block edges fall inside as well.
BLOCK_Y0, BLOCK_Y1, BLOCK_X0, BLOCK_X1 = 85, 484, 130, 590

# Rows that describe the background: one plain, one on a grid line.
PLAIN_ROW, LINE_ROW = 0, 3

# The three bottom rows carry a demodulation artefact that is not part
# of the picture and not part of the ring either. Left alone.
ARTEFACT_ROWS = (572, 575)

# Black text box, measured: the rows where the picture is black at x=340.
BOX_Y0, BOX_Y1 = 285, 323

WINDOW = 40      # luma samples either side of a run a donor must match
WINDOW_C = 20    # idem for chroma
WINDOW_C2 = 8    # narrower window for the best effort second pass
TOL = 3          # a second pass donor may differ this much, in code values


def circle_mask():
    """True where a sample is within MASK_HALF pixels of the ring."""
    yy, xx = np.mgrid[0:576, 0:720].astype(np.float64)
    dx, dy = xx - X0, yy - Y0
    f = (dx / RX) ** 2 + (dy / RY) ** 2 - 1.0
    grad = 2 * np.sqrt((dx / RX ** 2) ** 2 + (dy / RY ** 2) ** 2) + 1e-12
    return np.abs(f / grad) <= MASK_HALF


def runs_of(m):
    """The True stretches in a 1-D mask, as (first, last) pairs."""
    idx = np.nonzero(m)[0]
    out = []
    if len(idx) == 0:
        return out
    first = prev = idx[0]
    for v in idx[1:]:
        if v - prev > 1:
            out.append((first, prev))
            first = v
        prev = v
    out.append((first, prev))
    return out


def best_donor(planes, mask, todo, y, a, b, k):
    """Row that covers part of run [a,b] and differs least around it."""
    h, w = mask.shape
    lo, hi = max(0, a - k), min(w, b + 1 + k)
    best = None
    for yy in range(h):
        if yy == y:
            continue
        take = todo[y, a:b + 1] & (~mask[yy, a:b + 1])
        if not take.any():
            continue
        both = (~mask[y, lo:hi]) & (~mask[yy, lo:hi])
        if not both.any():
            continue
        dev = max(int(np.abs(p[y, lo:hi][both].astype(int) -
                             p[yy, lo:hi][both].astype(int)).max())
                  for p in planes)
        key = (dev, abs(yy - y))
        if best is None or key < best[0]:
            best = (key, yy, take.copy())
    return best


def fill(planes, mask, k, k2):
    """Replace masked samples by samples from other rows.

    First pass: only donors that agree exactly over a window of k
    samples either side of the run, nearest row first. Second pass, for
    what is left: the donor that differs least over a window of k2, as
    long as it stays within TOL. Whatever survives both is interpolated
    across the gap. The planes are filled together, so a donor has to
    match on all of them at once.
    """
    h, w = mask.shape
    out = [p.copy() for p in planes]
    todo = mask.copy()
    stuck = np.zeros_like(mask)
    stat = {"exact": 0, "near": 0, "ramp": 0, "left": 0, "worst": 0, "reach": 0}

    for y in range(h):
        while True:
            rs = runs_of(todo[y] & (~stuck[y]))
            if not rs:
                break
            a, b = rs[0]
            lo, hi = max(0, a - k), min(w, b + 1 + k)
            hit = False
            for dy in range(1, h):
                for yy in (y - dy, y + dy):
                    if yy < 0 or yy >= h:
                        continue
                    both = (~mask[y, lo:hi]) & (~mask[yy, lo:hi])
                    if not all(np.array_equal(p[y, lo:hi][both], p[yy, lo:hi][both])
                               for p in planes):
                        continue
                    take = todo[y, a:b + 1] & (~mask[yy, a:b + 1])
                    if not take.any():
                        continue
                    for p, o in zip(planes, out):
                        seg = o[y, a:b + 1]
                        seg[take] = p[yy, a:b + 1][take]
                        o[y, a:b + 1] = seg
                    todo[y, a:b + 1] &= ~take
                    stat["exact"] += int(take.sum())
                    stat["reach"] = max(stat["reach"], dy)
                    hit = True
                    break
                if hit:
                    break
            if not hit:
                stuck[y, a:b + 1] = True

    for y in range(h):
        for a, b in runs_of(todo[y]):
            while todo[y, a:b + 1].any():
                r = best_donor(planes, mask, todo, y, a, b, k2)
                if r is None or r[0][0] > TOL:
                    break
                (dev, dy), yy, take = r
                for p, o in zip(planes, out):
                    seg = o[y, a:b + 1]
                    seg[take] = p[yy, a:b + 1][take]
                    o[y, a:b + 1] = seg
                todo[y, a:b + 1] &= ~take
                stat["near"] += int(take.sum())
                stat["worst"] = max(stat["worst"], dev)
                stat["reach"] = max(stat["reach"], dy)
            left = todo[y, a:b + 1]
            if not left.any():
                continue
            if a > 0 and b + 1 < w:
                n = b - a + 2
                for p, o in zip(planes, out):
                    v0, v1 = float(p[y, a - 1]), float(p[y, b + 1])
                    ramp = np.round(v0 + (v1 - v0) * (np.arange(1, n) / n))
                    seg = o[y, a:b + 1]
                    seg[left] = ramp.astype(p.dtype)[left]
                    o[y, a:b + 1] = seg
                todo[y, a:b + 1] = False
                stat["ramp"] += int(left.sum())
            else:
                stat["left"] += int(left.sum())
    return out, todo, stat


def remove_circle(lines):
    """Take the ring out of a 576 x 1440 frame in 4:2:2."""
    luma = lines[:, 1::2].copy()
    cb = lines[:, 0::4].copy()
    cr = lines[:, 2::4].copy()
    mask = circle_mask()
    mask_c = mask[:, 0::2] | mask[:, 1::2]

    outside = np.ones((576, 720), bool)
    outside[BLOCK_Y0:BLOCK_Y1 + 1, BLOCK_X0:BLOCK_X1 + 1] = False
    outside_c = outside[:, 0::2] & outside[:, 1::2]

    # The background, as the picture itself has it.
    tmpl = np.tile(luma[PLAIN_ROW], (576, 1))
    for y in GRID_ROWS:
        tmpl[y, :] = luma[LINE_ROW]
    off = outside.copy()
    off[ARTEFACT_ROWS[0]:ARTEFACT_ROWS[1], :] = False
    bad = off & (~mask) & (luma != tmpl)
    if bad.any():
        raise SystemExit("background does not match the template in %d samples"
                         % int(bad.sum()))
    bad = outside_c & (~mask_c) & ((cb != 128) | (cr != 128))
    if bad.any():
        raise SystemExit("background chroma is not neutral in %d samples"
                         % int(bad.sum()))

    take = mask & outside
    luma[take] = tmpl[take]
    take_c = mask_c & outside_c
    cb[take_c] = 128
    cr[take_c] = 128
    print("  background: %d luma and %d chroma samples from the grid template"
          % (int(take.sum()), int(take_c.sum())))

    (luma,), left, stat = fill([luma], mask & (~outside), WINDOW, WINDOW_C2)
    if left.any():
        raise SystemExit("%d luma samples left unfilled" % int(left.sum()))
    print("  centre blocks, luma: %d exact, %d best effort, %d interpolated, "
          "furthest donor %d rows" % (stat["exact"], stat["near"], stat["ramp"],
                                      stat["reach"]))

    (cb, cr), left, stat = fill([cb, cr], mask_c & (~outside_c), WINDOW_C, WINDOW_C2)
    if left.any():
        raise SystemExit("%d chroma samples left unfilled" % int(left.sum()))
    print("  centre blocks, chroma: %d exact, %d best effort (worst %d code "
          "values off), %d interpolated, furthest donor %d rows"
          % (stat["exact"], stat["near"], stat["worst"], stat["ramp"], stat["reach"]))

    out = np.zeros_like(lines)
    out[:, 1::2] = luma
    out[:, 0::4] = cb
    out[:, 2::4] = cr
    return out


def remove_box_line(lines):
    """Take the centre grid line out of the black text box."""
    luma = lines[:, 1::2].copy()
    n = 0
    for y in range(BOX_Y0, BOX_Y1 + 1):
        black = luma[y, 340]
        if black != 16:
            raise SystemExit("row %d is not black inside the text box" % y)
        a = b = 720 // 2
        while luma[y, a - 1] != black:
            a -= 1
        while luma[y, b + 1] != black:
            b += 1
        if not 350 <= a <= 360 <= b <= 370:
            raise SystemExit("row %d: centre line runs from %d to %d" % (y, a, b))
        luma[y, a:b + 1] = black
        n += b - a + 1
    print("  text box: %d luma samples of the centre line set to black" % n)
    out = lines.copy()
    out[:, 1::2] = luma
    return out


def shipped_fubk4x3():
    """Decode the fubk4x3 tables that are in the tree, to compare with."""
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src",
                     "testcards")
    p = lambda e: os.path.join(d, "fubk4x3" + e)
    pal = np.frombuffer(read_table(p("_pal.inc.h")), "<u4")
    idx = np.frombuffer(read_table(p("_idx.inc.h")), "<u2")
    ln = np.frombuffer(read_table(p("_len.inc.h")), np.uint8)
    start = np.frombuffer(read_table(p("_start.inc.h")), "<u4")
    line = np.frombuffer(read_table(p("_line.inc.h")), "<u2")
    frame = np.zeros((576, 1440), np.uint8)
    for y in range(576):
        li = line[y]
        words = []
        for k in range(start[li], start[li + 1]):
            words += [pal[idx[k]]] * ln[k]
        if len(words) != 360:
            raise SystemExit("fubk4x3 line %d holds %d words" % (y, len(words)))
        frame[y] = np.array(words, "<u4").view(np.uint8)
    return frame


def main():
    print("FUBK 4:3 without the circle")
    frame, pad = build_frame_hacktv(SRC_FILE, LUMA_LEVELS, CHROMA_LEVELS,
                                    max_error=MAX_ERROR)
    print("  %s: demodulated from composite" % SRC_FILE)
    if not np.array_equal(frame, shipped_fubk4x3()):
        raise SystemExit("the rebuilt frame differs from the shipped fubk4x3 "
                         "tables; the source or the conversion has changed")
    print("  same as the shipped FUBK 4:3, byte for byte")

    out = remove_circle(frame)
    out = remove_box_line(out)
    print("  changed: %d of %d bytes (%.2f percent)"
          % (int((out != frame).sum()), out.size, 100.0 * (out != frame).mean()))

    cfg = {"title": "FUBK 4:3 without the circle",
           "file": SRC_FILE,
           "note": "as the FUBK 4:3, but without the centre circle and "
                          "without the centre line in the text box; built by "
                          "tools/make_fubk_nocircle.py"}
    encode_and_write(NAME, cfg, [out], pad)


if __name__ == "__main__":
    sys.exit(main())

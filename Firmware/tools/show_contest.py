"""
Recomputes the layout of the contest page, without a board.

    python tools/show_contest.py
    python tools/show_contest.py --text1 PA0ABC --text2 JO32AA --number 1234
    python tools/show_contest.py --png after.png --hard before.png
    python tools/show_contest.py --measure

Why this exists: the contest page puts large text in three fixed boxes,
and the scale from the settings is a MAXIMUM. If the text does not fit,
textPrepareScaled() turns it down itself. That is exactly the sort of
thing you only see on the screen, and then you stand there squinting at
whether it is right or not. This script reads src/font_data.h, does the
same sum as the firmware and says what comes out.

It also draws a PNG with the three boxes in it, so you can see whether
the proportions are right before you flash. With --hard the picture
comes next to it as it WAS: rasters scaled up straight, so with the
steps in them.

--measure does the two checks that those edges are all about:

  1  HOW BIG IS THE STEP STILL. Per picture row the edge position is
     computed back to a sixteenth of a pixel, and then compared with the
     same edge TWO rows further on, that is the next row shown in the
     same field. That is the jump you see.
  2  DOES IT FIT IN 64 US. Per picture row what the drawing layer does
     is counted and converted to instructions. See COSTS below for where
     those numbers come from.

This code deliberately does EXACTLY the same as textDrawRow() in
src/textoverlay.cpp.
"""

import argparse
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_H = os.path.join(ROOT, "src", "font_data.h")

SCREEN_W, SCREEN_H = 720, 576

# Must be equal to the constants in src/testcards/contest.cpp.
BOX_X0, BOX_X1 = 20, 700
BOXES = {"text top": (40, 150), "number": (200, 376), "text bottom": (426, 536)}


def read_fonts():
    s = io.open(FONT_H, encoding="utf-8").read()
    glyphpat = re.compile(r"[{](GLYPH_OSLASH|'[^']*'),\s*(\d+),\s*[{]([^}]*)[}][}]")

    def glyphs(name):
        m = re.search(r"static const Glyph FONT_" + name + r"_G\[\] = [{](.*?)\n[}];", s, re.S)
        out = {}
        for mm in glyphpat.finditer(m.group(1)):
            char, cols, rows = mm.group(1), int(mm.group(2)), mm.group(3)
            if char == "GLYPH_OSLASH":
                c = "Ø"
            else:
                c = char.strip("'")
                if c.startswith("\\x"):
                    c = chr(int(c[2:], 16))
                elif c.startswith("\\"):
                    c = c[1:]
            out[c] = (cols, [int(v.strip(), 16) for v in rows.split(",") if v.strip()])
        return out

    m = re.search(r"static const Font FONTS\[\] = [{](.*?)\n[}];", s, re.S)
    out = []
    for mm in re.finditer(r'[{]"(\w+)", FONT_\w+_G, (\d+), (\d+), (\d+), (\d+), (\d+), (\d+)[}]',
                          m.group(1)):
        name = mm.group(1)
        numbers = [int(v) for v in mm.groups()[1:]]
        out.append((name, glyphs(name), dict(zip(("count", "rows", "sx", "sy", "digits", "gap"),
                                                 numbers))))
    return out


def measure(text, g, f):
    """Column widths and the top and bottom row with ink, as textPrepare."""
    cols, top, bottom = [], f["rows"], -1
    for ch in text.upper():
        if ch not in g:
            ch = " " if " " in g else list(g)[0]
        c, rows = g[ch]
        cols.append(f["digits"] if ch.isdigit() else c)
        for i, v in enumerate(rows):
            if v:
                top = min(top, i)
                bottom = max(bottom, i)
    if bottom < 0:
        top, bottom = 0, f["rows"] - 1
    return cols, top, bottom


def fit_scale(cols, top, bottom, f, scale, space_w, space_h):
    """The same limiting as textPrepareScaled()."""
    while scale > 1:
        wide = sum(cols) * f["sx"] * scale + (len(cols) - 1) * f["gap"] * scale
        high = (bottom - top + 1) * f["sy"] * scale
        if wide <= space_w and high <= space_h:
            break
        scale -= 1
    return scale


# --------------------------------------------------------------------------
# The drawing routine. Same sum as textDrawRow() in src/textoverlay.cpp.
# --------------------------------------------------------------------------

# This many font columns may an edge shift between two rows and still
# count as the SAME edge. Must be equal to EDGE_MAX_STEP in
# src/textoverlay.cpp.
EDGE_MAX_STEP = 2

BLACK, WHITE = 16, 235     # BT.601, as GFX_BLACK and GFX_WHITE in gfx.h

# COSTS: instructions per piece of work, counted in the disassembly of
# the built firmware (arm-none-eabi-objdump -dS on textoverlay.cpp,
# -mcpu=cortex-m33 -O2, the same flags as platformio.ini).
#
#   span pixel  the loop in fillSpanSolid() is 21 instructions per TWO
#               pixels: 11 for the even one (three bytes, Y and both
#               chroma) and 10 for the odd one (Y only)
#   part pixel  edgeBlendPixel() with the test around it
#   run         per piece of ink: two interpolations, the span bounds
#               and the loop check
#   char/search edgeFillCache(): fixed cost per character, plus per
#               edgeNeighbour() search. That function is in there fully
#               unrolled and tests at most five places, one bit test
#               per place. 25 is the longest path; whoever finds an
#               edge at d=0 is out in about five. The four masks
#               around it are in I_CHAR.
#   bit loop    the OLD loop, per bit position it walks past
#   black       contestRenderRow() fills 360 words, 2 instructions each
#
# Assumed: one instruction per clock. On a Cortex-M33 a taken branch
# costs more, so this is a LOWER bound; the firmware measures it itself
# and reports it over the serial port (key d). That remains the proof.
I_SPANPIX, I_PARTPIX, I_RUN = 10.5, 18, 32
I_CHAR, I_SEARCH, I_FILLRUN, I_BITLOOP = 20, 25, 12, 7
I_BLACK = 720
CLOCK_MHZ = 108.0          # board_build.f_cpu in platformio.ini
BUDGET_US = 64.0           # a PAL line, see VIDEO_LINE_BUDGET_US


def runs(bits):
    """A font row in contiguous pieces of ink: (first column, column after)."""
    out, k = [], 0
    while k < 32:
        if bits & (1 << (31 - k)):
            l = k
            while k < 32 and (bits & (1 << (31 - k))):
                k += 1
            out.append((l, k))
        else:
            k += 1
    return out


def bit(bits, c):
    return 1 if (0 <= c < 32 and (bits & (1 << (31 - c)))) else 0


def left_edges(v):
    """Bit 31-c set where column c is a left edge; the whole row at once."""
    return v & ~(v >> 1) & 0xFFFFFFFF


def right_edges(v):
    """The same for the right edges: column c-1 has ink, column c does not."""
    return (v >> 1) & ~v & 0xFFFFFFFF


def neighbour_edge(edges, e, mx):
    """Which edge in the neighbouring row belongs to column boundary e?

    Search from e in BOTH directions, nearest first. None means: within
    mx there is no edge. That is a corner and it should stay hard.
    """
    for d in range(0, mx + 1):
        for side in (0, 1):
            if d == 0 and side:
                break
            c = e - d if side else e + d
            if 0 <= c < 32 and (edges >> (31 - c)) & 1:
                return c
    return None


def boundary(edges, e, mx):
    """Edge position on the boundary with the neighbouring row, in HALF font columns."""
    p = neighbour_edge(edges, e, mx)
    return 2 * e if p is None else e + p


def doubled_rows(text, g, f):
    """Does this font stand doubled in the table? Same test as textPrepare.

    The PM5544 is 7x15 and is expanded by make_fonts.py with NEAREST to
    14x30, so there every odd row is a copy of the even row before it
    and every odd column a copy of the even column beside it. Those rows
    are then demonstrably superfluous and two font rows may be treated
    as one control point.
    """
    if f["rows"] % 2:
        return 1
    for ch in text:
        _, rows = g[ch if ch in g else " "]
        for r in range(0, f["rows"], 2):
            if rows[r] != rows[r + 1] or (((rows[r] >> 1) ^ rows[r]) & 0x55555555):
                return 1
    return 2


def draw_row(im, costs, text, g, f, sc, x0, ytop, cols, aa):
    """Puts a row into im (luma 16..235) and counts the work in costs."""
    sx, sy = f["sx"] * sc, f["sy"] * sc
    gap = f["gap"] * sc
    step = doubled_rows(text, g, f) if aa else 1
    # The slack stays in FONT columns, also for a doubled font.
    mx = EDGE_MAX_STEP
    empty = dict(spanpix=0, partpix=0, runs=0, char=0, search=0, fillrun=0, bitloop=0)
    x = x0
    for i, ch in enumerate(text):
        gc, rows = g[ch if ch in g else " "]
        gx = x + ((cols[i] - gc) // 2) * sx
        for r in range(0, f["rows"], step):
            bits = rows[r]
            top = rows[r - step] if r >= step else 0
            bot = rows[r + step] if r + step < f["rows"] else 0
            # Two shortcuts that give exactly the same answer as the
            # search loop; see edgeFillCache() in textoverlay.cpp.
            top_same, bot_same = top in (0, bits), bot in (0, bits)
            topL, topR = left_edges(top), right_edges(top)
            botL, botR = left_edges(bot), right_edges(bot)
            pieces = runs(bits)
            band_h = step * sy                # picture rows between two control points
            first = ytop + r * sy
            k0 = costs[first] if 0 <= first < SCREEN_H else empty
            if aa and pieces:
                # edgeFillCache() only runs on the FIRST picture row of
                # this font row; the rest only read the result out.
                k0["char"] += 1
                k0["search"] += len(pieces) * ((0 if top_same else 2) + (0 if bot_same else 2))
                k0["fillrun"] += len(pieces)
            for (l, e) in pieces:
                if aa:
                    lt = gx * 16 + (l * sx * 16 if top_same else boundary(topL, l, mx) * sx * 8)
                    lb = gx * 16 + (l * sx * 16 if bot_same else boundary(botL, l, mx) * sx * 8)
                    rt = gx * 16 + (e * sx * 16 if top_same else boundary(topR, e, mx) * sx * 8)
                    rb = gx * 16 + (e * sx * 16 if bot_same else boundary(botR, e, mx) * sx * 8)
                else:
                    lt = lb = (gx + l * sx) * 16
                    rt = rb = (gx + e * sx) * 16
                for dy in range(band_h):
                    y = first + dy
                    if not (0 <= y < SCREEN_H):
                        continue
                    q = ((dy << 16) + band_h // 2) // band_h
                    XL = lt + (((lb - lt) * q + 32768) >> 16)
                    XR = rt + (((rb - rt) * q + 32768) >> 16)
                    span(im, costs[y], y, XL, XR)
            if not aa and pieces:
                # The old loop walks the bits one by one, up to the last
                # column with ink, and does that per PICTURE row.
                last = max(e for (_, e) in pieces)
                for dy in range(sy):
                    y = first + dy
                    if 0 <= y < SCREEN_H:
                        costs[y]["bitloop"] += last
        x += cols[i] * sx + gap


def put(im, k, x, y, cov16):
    """A pixel with coverage cov16/16, luma mixed with what is there."""
    if not (0 <= x < SCREEN_W and 0 <= y < SCREEN_H):
        return
    bg = int(im[y, x])
    im[y, x] = (bg * (16 - cov16) + WHITE * cov16 + 8) >> 4
    k["partpix" if cov16 < 16 else "spanpix"] += 1


def span(im, k, y, XL, XR):
    """A piece of ink from XL to XR, both in sixteenths of a pixel."""
    if XR <= XL:
        return
    k["runs"] += 1
    a, b = XL >> 4, XR >> 4
    if a == b:
        put(im, k, a, y, XR - XL)
        return
    if XL & 15:
        put(im, k, a, y, 16 - (XL & 15))
        a += 1
    for px in range(a, b):
        put(im, k, px, y, 16)
    if XR & 15:
        put(im, k, b, y, XR & 15)


def build(fontidx, rows, aa):
    """The whole picture, plus per picture row what the drawing cost."""
    import numpy as np
    name, g, f = read_fonts()[fontidx]
    im = np.full((SCREEN_H, SCREEN_W), BLACK, np.uint8)
    costs = [dict(spanpix=0, partpix=0, runs=0, char=0, search=0, fillrun=0, bitloop=0)
             for _ in range(SCREEN_H)]
    for box, text, wanted in rows:
        y0, y1 = BOXES[box]
        space_w, space_h = (BOX_X1 - BOX_X0) - 6, y1 - y0
        cols, top, bottom = measure(text, g, f)
        sc = fit_scale(cols, top, bottom, f, wanted, space_w, space_h)
        sx, sy = f["sx"] * sc, f["sy"] * sc
        gap = f["gap"] * sc
        textW = sum(cols) * sx
        x = BOX_X0 + ((BOX_X1 - BOX_X0) - (textW + (len(cols) - 1) * gap)) // 2
        ytop = y0 + ((y1 - y0) - (bottom - top + 1) * sy) // 2 - top * sy
        # At true size the firmware does nothing new; see textDrawRow().
        draw_row(im, costs, text.upper(), g, f, sc, x, ytop, cols, aa and sy >= 2)
    return name, im, costs


def us_per_line(k, aa):
    n = I_BLACK + k["spanpix"] * I_SPANPIX + k["runs"] * (I_RUN if aa else 6)
    if aa:
        n += k["partpix"] * I_PARTPIX + k["char"] * I_CHAR + k["search"] * I_SEARCH \
             + k["fillrun"] * I_FILLRUN
    else:
        n += k["bitloop"] * I_BITLOOP
    return n / CLOCK_MHZ


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--text1", default="PE5PVB")
    p.add_argument("--text2", default="JO21QM")
    p.add_argument("--number", type=int, default=1)
    # Default equal to CFG_CONTEST_SCALE_TEXT and _NUMBER in src/config.h.
    p.add_argument("--scale-text", type=int, default=5)
    p.add_argument("--scale-number", type=int, default=9)
    p.add_argument("--png", help="write an image of the result")
    p.add_argument("--hard", help="the same, but with the rasters scaled up straight (as it WAS)")
    p.add_argument("--measure", action="store_true",
                   help="step size and time per picture line, before and after")
    p.add_argument("--font", type=int, default=0, help="0 = PM5544, 1 = PM8546")
    a = p.parse_args()

    rows = (("text top", a.text1, a.scale_text),
            ("number", "%04d" % a.number, a.scale_number),
            ("text bottom", a.text2, a.scale_text))

    for name, g, f in read_fonts():
        print("=== %s: %d rows, scale %dx%d, digit width %d, letter spacing %d"
              % (name, f["rows"], f["sx"], f["sy"], f["digits"], f["gap"]))
        for box, text, wanted in rows:
            y0, y1 = BOXES[box]
            space_w, space_h = (BOX_X1 - BOX_X0) - 6, y1 - y0
            cols, top, bottom = measure(text, g, f)
            sc = fit_scale(cols, top, bottom, f, wanted, space_w, space_h)
            wide = sum(cols) * f["sx"] * sc + (len(cols) - 1) * f["gap"] * sc
            high = (bottom - top + 1) * f["sy"] * sc
            flag = "" if sc == wanted else "   <-- turned down from %d" % wanted
            print("   %-12s %-10s scale %d -> %3d x %3d px, box %3d x %3d%s"
                  % (box, '"' + text + '"', sc, wide, high, space_w, space_h, flag))
        print()

    for path, aa in ((a.png, True), (a.hard, False)):
        if not path:
            continue
        from PIL import Image
        name, im, _ = build(a.font, rows, aa)
        Image.fromarray(im).save(path)
        print("%s written (%s, %s)"
              % (path, name, "edges followed" if aa else "raster scaled up straight"))

    if a.measure:
        measure_all(rows)
    return 0


def edge_positions(row):
    """The edges of a picture row back to a sixteenth of a pixel.

    An edge that lies between two pixels shows itself as a pixel with an
    intermediate value: that one is covered for cov/16 part. That is how
    it can be read back out of the PICTURE where the edge really lay,
    and that is exactly what the television gets to see as well.
    """
    c = [(int(v) - BLACK) / float(WHITE - BLACK) for v in row]
    out, k, n = [], 0, len(c)
    while k < n:
        if c[k] > 0.002:
            i0 = k
            while k < n and c[k] > 0.002:
                k += 1
            i1 = k - 1
            if i1 > i0:
                out.append((i0 + (1 - c[i0]), i1 + c[i1]))
            else:
                out.append((i0 + 0.5 - c[i0] / 2, i0 + 0.5 + c[i0] / 2))
        else:
            k += 1
    return out


def jumps(im, y0, y1):
    """How far an edge shifts to the next row IN THE SAME FIELD.

    A field only shows every other picture row, so the row that comes
    after this one lies two further on. Rows where the number of pieces
    of ink changes are skipped: something begins or ends there, and that
    is a corner and not a step.
    """
    out = []
    for y in range(y0, y1 - 2):
        a, b = edge_positions(im[y]), edge_positions(im[y + 2])
        if not a or len(a) != len(b):
            continue
        for (l1, r1), (l2, r2) in zip(a, b):
            out.append(abs(l2 - l1))
            out.append(abs(r2 - r1))
    return sorted(v for v in out if v > 0.01)


def measure_all(rows):
    import collections
    ny0, ny1 = BOXES["number"]

    print("=== widest font row: how many separate pieces of ink?")
    for name, g, f in read_fonts():
        mx, where = 0, None
        for ch, (_, glyph_rows) in g.items():
            for r, bits in enumerate(glyph_rows):
                n = len(runs(bits))
                if n > mx:
                    mx, where = n, "%r row %d" % (ch, r)
        print("   %-8s %d pieces (%s); RAND_MAX_RUNS in textoverlay.cpp must be >= %d"
              % (name, mx, where, mx))

    for fi, (name, _, _) in enumerate(read_fonts()):
        print()
        print("=== %s" % name)
        images = {}
        for aa in (False, True):
            _, im, costs = build(fi, rows, aa)
            images[aa] = im
            us = [us_per_line(k, aa) for k in costs]
            heaviest = max(range(SCREEN_H), key=lambda y: us[y])
            print("   %-16s time per picture line: heaviest %5.1f us (row %d), average %5.1f us"
                  "; budget %.0f us, so %2.0f%% used"
                  % ("raster scaled up" if not aa else "edges followed",
                     us[heaviest], heaviest, sum(us) / len(us), BUDGET_US,
                     100.0 * us[heaviest] / BUDGET_US))
        for aa in (False, True):
            d = jumps(images[aa], ny0, ny1)
            h = collections.Counter(round(v, 1) for v in d)
            top = ", ".join("%.1f px: %dx" % (k, v) for k, v in sorted(h.items())[-4:])
            print("   %-16s edge jump in the number box: largest %.1f px, %d out of %d above 4 px"
                  % ("raster scaled up" if not aa else "edges followed",
                     max(d) if d else 0, sum(1 for v in d if v > 4), len(d)))
            print("   %-16s   largest four: %s" % ("", top))


if __name__ == "__main__":
    sys.exit(main())

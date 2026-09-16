#!/usr/bin/env python3
"""Renders the Inter lettering for the contest page, at 90 row capitals:
what the raster fit rule reached in the 110 row text boxes of the
contest page (scale 3 on 30 font rows). No rings: the page is black and
needs none. Written to contest_font.h.

Inter's OWN ticker face, at 60 row capitals with the ring per glyph the
transparent mode lives off, is built by the SAME build_into() this
script defines, but called from make_ticker_font.py rather than from
here: that script also builds the Doto ticker face and writes both into
one shared src/ticker_font.h.

Exactly the characters of the raster fonts are made, no more and no
less: CHARS in make_fonts.py, ending on the slashed O. The contest page
falls back on the raster path when a line runs too wide and the ticker
offers the raster faces next to this one, so a character that only one
side knew would silently turn into a space on the other. The firmware
folds lower case to upper case, exactly as the raster fonts do.

The capital height is measured on the H, which is flat at both ends;
measuring on the whole alphabet would let the tails of the Q and the J
drag the capitals visibly under the asked size. Those tails simply
reach below the band, as the face draws them. Everything else about a
glyph is taken from the face itself: its advance and left side bearing
travel along, so the natural fit of the letters is kept and no fixed
letter spacing is bolted on.

Each glyph is cropped to its own ink both ways and stored as RUNS, not
as bare coverage bytes: see contest_rle.py for the form and for why a
byte per pixel drained the ring buffer to black. The vertical position
returns through yoff, counted from the top of the capitals, so a glyph
below the baseline (the underscore) lands where it belongs.

The ring, where asked for, mirrors the widths the raster ticker
measured on the board: two pixels of solid black against the ink (255)
and one more blended halfway (128), square dilation like widen() plus
the row join reach. The margin of 3 is EDGE_HARD + EDGE_SOFT, and
ticker.cpp carries a static assert that leans on it.

Run from the tools directory; the face files sit next to this script.
"""

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from contest_rle import emit_rle

# Byte for byte the CHARS of make_fonts.py, so every character that can
# be typed exists in the raster faces and in these run faces alike.
CHARSET = " !\"&'()+,-./0123456789:;=?ABCDEFGHIJKLMNOPQRSTUVWXYZ_Ø"
FONT_FILE = "Inter-Regular.ttf"

RING_HARD = 2
RING_SOFT = 1
RING = RING_HARD + RING_SOFT

CANVAS = (500, 400)
ORIGIN = (100, 300)


def ink_box(font, ch):
    img = Image.new("L", CANVAS, 0)
    ImageDraw.Draw(img).text(ORIGIN, ch, font=font, fill=255, anchor="ls")
    return img, img.getbbox()


def caps_band(size):
    """Top and bottom of the flat capitals, measured on the H."""
    font = ImageFont.truetype(FONT_FILE, size)
    _, b = ink_box(font, "H")
    return b[1], b[3]


def dilate(mask, n):
    """Square dilation, the same reach the raster ring has: widen()
    works horizontally and the row join vertically, each over the full
    distance, so the window is a square and not a diamond."""
    h, w = mask.shape
    p = np.pad(mask, n)
    out = np.zeros_like(mask)
    for dy in range(2 * n + 1):
        for dx in range(2 * n + 1):
            out |= p[dy:dy + h, dx:dx + w]
    return out


def ring_of(data, w, h):
    """The ring coverage around one glyph: 255 in the hard ring, 128 in
    the soft ring, 0 on the ink itself; the ink lifts over it afterwards
    so its antialiased fringe blends against black, exactly what the
    raster path achieves with its draw order."""
    ink = np.frombuffer(data, np.uint8).reshape(h, w) > 127
    big = np.zeros((h + 2 * RING, w + 2 * RING), bool)
    big[RING:RING + h, RING:RING + w] = ink
    hard = dilate(big, RING_HARD)
    soft = dilate(big, RING) & ~hard
    cov = hard.astype(np.uint8) * 255
    cov[soft] = 128
    cov[big] = 0
    # rleDarkenRow() halves the luma without a multiply because every
    # edge byte in a ring stream is exactly 128 (255 always becomes a
    # solid run in the encoder). Anything else here must fail the build.
    assert set(np.unique(cov)) <= {0, 128, 255}
    return cov.tobytes()


def build_into(f, cap_target, prefix, struct, rings):
    """The CAP/TOP/BOTTOM constants, the glyph tokens and the struct
    table for one face, written into an already open file. Split out of
    build() below so a caller merging several faces into one header
    (see tools/make_ticker_font.py) can write the shared preamble once
    and call this once per face."""
    size = cap_target
    while True:
        y0, y1 = caps_band(size + 1)
        if y1 - y0 > cap_target:
            break
        size += 1
    font = ImageFont.truetype(FONT_FILE, size)
    cap_top, cap_bot = caps_band(size)
    print(f"{prefix}: point size {size}, capitals {cap_bot - cap_top} rows "
          f"(target {cap_target})")

    glyphs = []
    for ch in CHARSET:
        adv = max(1, round(font.getlength(ch)))
        if ch == " ":
            glyphs.append((ch, 0, 0, 0, 0, adv, b""))
            continue
        img, b = ink_box(font, ch)
        crop = img.crop(b)
        lsb = b[0] - ORIGIN[0]
        yoff = b[1] - cap_top
        glyphs.append((ch, crop.width, crop.height, yoff, lsb, adv, crop.tobytes()))

    f.write(f"static const int {prefix}_CAP = {cap_bot - cap_top};\n")
    top = min(y for _, _, h, y, _, _, _ in glyphs if h)
    bot = max(y + h for _, _, h, y, _, _, _ in glyphs if h)
    f.write(f"static const int {prefix}_TOP = {top};\n")
    f.write(f"static const int {prefix}_BOTTOM = {bot};\n\n")
    total = 0
    for i, (ch, w, h, yoff, lsb, adv, data) in enumerate(glyphs):
        if not data:
            continue
        total += emit_rle(f, f"{prefix}_{i}", data, w, h)
        if rings:
            total += emit_rle(f, f"{prefix}_{i}_RING",
                              ring_of(data, w, h), w + 2 * RING, h + 2 * RING)
    # The struct itself lives in src/runglyph.h, so a file that only
    # handles pointers can see the fields without pulling the tables
    # in; this header only fills a table of them.
    f.write(f"\nstatic const {struct} {prefix}[{len(glyphs)}] = {{\n")
    for i, (ch, w, h, yoff, lsb, adv, data) in enumerate(glyphs):
        code = 0x01 if ch == "Ø" else ord(ch)
        off = f"{prefix}_{i}_O" if data else "nullptr"
        rle = f"{prefix}_{i}_R" if data else "nullptr"
        line = f"    {{{code}, {w}, {h}, {yoff}, {lsb}, {adv}, {off}, {rle}"
        if rings:
            roff = f"{prefix}_{i}_RING_O" if data else "nullptr"
            rrle = f"{prefix}_{i}_RING_R" if data else "nullptr"
            line += f", {roff}, {rrle}"
        f.write(line + "},\n")
    f.write("};\n")
    print(f"  band {top}..{bot}, {total} bytes of tokens")


def build(cap_target, out, prefix, struct, rings, whom, glyph_h):
    with open(out, "w", newline="\n") as f:
        f.write("// Generated by tools/make_contest_font.py -- do not edit.\n")
        f.write(f"// Inter (SIL OFL 1.1) for {whom}, as runs, rendered\n")
        f.write("// at the final size so the firmware never scales it. See\n")
        f.write("// tools/contest_rle.py for the run form and the generator for\n")
        f.write("// what yoff, lsb and adv mean.\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f'#include "{glyph_h}"\n\n')
        build_into(f, cap_target, prefix, struct, rings)
    print(f"  -> {out}")


def main():
    # Also handed out to Mixed bars' own line, through
    # testcards/contest_face.h: same face, same size, one table.
    build(90, "../src/testcards/contest_font.h", "CONTEST_FONT", "ContestGlyph",
          rings=False, whom="the contest page and the Mixed bars card",
          glyph_h="../runglyph.h")
    # The ticker face (TICKER_FONT) is no longer built here: it shares
    # src/ticker_font.h with the Doto face, see make_ticker_font.py.


if __name__ == "__main__":
    main()

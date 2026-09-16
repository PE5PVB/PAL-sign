"""
Turns a TFT_eSPI smooth font into a header the display can draw.

    python tools/make_smooth_font.py FONT28.h src/display_font28.h DISPFONT28
    python tools/make_smooth_font.py FONT16.h src/display_font16.h DISPFONT16

The input is a .vlw font written out as a C byte array, which is what the
TFT_eSPI "smooth font" tools produce. Only the printable ASCII range is
kept: the source font carries 704 glyphs and nearly all of them are for
alphabets this panel never shows.

WHY A SMOOTH FONT AT ALL. A one bit face has every edge either on or off,
so the diagonals and the round bowls turn into visible steps. A smooth
font carries an alpha value per pixel instead, so the renderer can blend
the edge into the background and the shape reads as it was drawn.

THE .VLW LAYOUT, taken from the file itself and confirmed against the
values it produces:

    header   6 x int32, big endian
             glyph count, version, point size, 0, ascent, descent
    table    glyph count x 7 x int32, big endian
             code point, height, width, xAdvance, dY, dX, 0
    bitmaps  in the same order as the table, height x width bytes each,
             one alpha byte per pixel

dY is the distance from the baseline up to the top row of the bitmap and
dX the left side bearing, both in pixels.
"""

import argparse
import io
import os
import re
import struct
import sys

FIRST, LAST = 32, 126

# Kept on top of printable ASCII: the copyright sign, for the version
# line of the splash. The source font carries it at Latin-1 0xA9 and
# the firmware writes it as "\xA9" in the string.
KEEP_EXTRA = {0xA9}

NL = chr(10)
QUOTE = chr(39)
BACKSLASH = chr(92)


def read_vlw(path):
    """Pull the byte array out of the C header and parse it."""
    text = io.open(path, encoding="utf-8", errors="replace").read()
    raw = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", text))
    count, version, size, _, ascent, descent = struct.unpack(">6i", raw[:24])
    glyphs = []
    offset = 24 + count * 28
    for i in range(count):
        code, h, w, adv, dy, dx, _ = struct.unpack(">7i", raw[24 + i * 28:24 + i * 28 + 28])
        glyphs.append({
            "code": code, "h": h, "w": w, "adv": adv, "dy": dy, "dx": dx,
            "bitmap": raw[offset:offset + h * w],
        })
        offset += h * w

    # AFTER THE BITMAPS COME THE TWO NAMES, each a uint16 length followed
    # by the characters, and then one byte that the format does not
    # explain. In this file that tail is 41 bytes and it parses exactly,
    # which is the check that the bitmaps were read at the right lengths:
    # get one glyph size wrong and everything after it shifts, and the
    # tail no longer lands where it should.
    name = ""
    tail = raw[offset:]
    try:
        n1 = struct.unpack(">H", tail[0:2])[0]
        name = tail[2:2 + n1].decode("ascii")
        n2 = struct.unpack(">H", tail[2 + n1:4 + n1])[0]
        used = 4 + n1 + n2 + 1
    except Exception:
        used = -1
    if used != len(tail):
        raise SystemExit("the file does not parse: %d bytes of bitmaps, then %d left "
                         "that are not the name block" % (offset - 24 - len(glyphs) * 28,
                                                          len(tail)))
    return version, size, ascent, descent, glyphs, name


def char_literal(code):
    """The character as a C literal, with the two that need escaping;
    anything above ASCII goes out as a plain number, which the uint8_t
    field takes just as well."""
    if code > LAST:
        return "0x%02X" % code
    ch = chr(code)
    if ch == QUOTE:
        return QUOTE + BACKSLASH + QUOTE + QUOTE
    if ch == BACKSLASH:
        return QUOTE + BACKSLASH + BACKSLASH + QUOTE
    return QUOTE + ch + QUOTE


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input", help="the .h with the vlw byte array")
    p.add_argument("output", help="where to write the header")
    p.add_argument("prefix", help="name of the table, for example DISPFONT16")
    a = p.parse_args()

    version, size, ascent, descent, glyphs, name = read_vlw(a.input)
    kept = [g for g in glyphs if FIRST <= g["code"] <= LAST or g["code"] in KEEP_EXTRA]
    have = {g["code"] for g in kept}

    # THE SPACE IS NOT IN THE FILE, and that is normal for a .vlw: it has
    # no ink, so the converter that made it left it out. Its width has to
    # come from somewhere, and two ways of arriving at it agree: a quarter
    # of the em, and half the advance of a digit. Both are printed at the
    # end, so a face where they disagree is noticed and not guessed at.
    digit = [g["adv"] for g in kept if g["code"] == ord("0")]
    quarter_em = (size + 2) // 4
    half_digit = (digit[0] + 1) // 2 if digit else quarter_em
    space_adv = quarter_em
    if FIRST in have:
        space_adv = next(g["adv"] for g in kept if g["code"] == FIRST)

    blob = bytearray()
    rows = []
    for g in sorted(kept, key=lambda g: g["code"]):
        rows.append((g["code"], g["w"], g["h"], g["adv"], g["dx"], g["dy"], len(blob)))
        blob += g["bitmap"]

    # How far the ink really reaches, over the glyphs that were kept. Not
    # the same as ascent and descent; see smoothfont.h.
    ink_up = max(g["dy"] for g in kept)
    ink_down = max(g["h"] - g["dy"] for g in kept)

    P = a.prefix
    saved = (sum(g["h"] * g["w"] for g in glyphs) - len(blob)) // 1024
    out = []
    out.append("// GENERATED by tools/make_smooth_font.py -- do not edit by hand.")
    out.append("//")
    out.append("// %s at %d points, as a smooth font: one alpha byte per pixel"
               % (name, size))
    out.append("// instead of one bit, so the renderer can blend the edges.")
    out.append("//")
    out.append("// Kept: printable ASCII and the copyright sign, %d glyphs of the"
               % len(rows))
    out.append("// %d in the source font." % len(glyphs))
    out.append("// The rest are alphabets this panel never shows and they would")
    out.append("// have cost %d kB of flash for nothing." % saved)
    out.append("#pragma once")
    out.append("")
    out.append('#include "smoothfont.h"')
    out.append("")
    out.append("static const SmoothGlyph %s_GLYPHS[] = {" % P)
    for code, w, h, adv, dx, dy, off in rows:
        out.append("    {%s, %2d, %2d, %2d, %3d, %3d, %6d},"
                   % (char_literal(code), w, h, adv, dx, dy, off))
    out.append("};")
    out.append("")
    out.append("static const uint8_t %s_PIXELS[] = {" % P)
    for i in range(0, len(blob), 24):
        out.append("    " + " ".join("0x%02X," % v for v in blob[i:i + 24]))
    out.append("};")
    out.append("")
    out.append("// Metrics straight out of the file, except the space, which carries")
    out.append("// no ink and so is not in the file at all, and the ink band, which")
    out.append("// is measured over the glyphs that were kept. See smoothfont.h for")
    out.append("// why the band and the nominal line are not the same thing.")
    out.append("static const SmoothFont %s = {" % P)
    out.append("    %s_GLYPHS, %s_PIXELS, %d, %d, %d, %d, %d, %d, %d,"
               % (P, P, len(rows), ascent, descent, ascent + descent, space_adv,
                  ink_up, ink_down))
    out.append("};")

    io.open(a.output, "w", encoding="utf-8", newline=NL).write(NL.join(out) + NL)

    print("%s: %d glyphs, %d alpha bytes, %d bytes of flash in total"
          % (os.path.basename(a.output), len(rows), len(blob),
             len(blob) + len(rows) * 12 + 32))
    print("  ascent %d, descent %d, so a line is %d rows"
          % (ascent, descent, ascent + descent))
    print("  ink reaches %d up and %d down, so the band is %d rows"
          % (ink_up, ink_down, ink_up + ink_down))
    print("  space %d, from a quarter of the em (%d) and half a digit (%d)"
          % (space_adv, quarter_em, half_digit))
    return 0


if __name__ == "__main__":
    sys.exit(main())

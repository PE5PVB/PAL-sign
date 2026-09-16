"""
Builds src/font_data.h with every font the firmware knows.

    python tools/make_fonts.py --nimbus NimbusSanL-Bol.otf

Two of them:

  PM5544     the font traced from a broadcast PM5544, 7x15 at scale 2.
             The source table sits separately in tools/font_pm5544.h;
             reading it out of src/font_data.h was not possible, as
             that file is precisely what gets overwritten here.
  PM8546     Nimbus Sans L Bold at 26 point, proportional. It is called
             that because it comes close to the real PM8546 font; it is
             NOT that font. The character set from the PM8546 PROM has
             been pulled out, see tools/extract_pm8546_font.py, but it
             is not in the firmware, because resampling made it
             unreadable and at full size it does not fit.

THE TWO ARE LAID OUT DIFFERENTLY, and that is deliberate:

  PM5544  FIXED width, exactly like the real generator. Every character
          gets the full cell of 14 px, only -:.,!() get 6. Letter
          spacing 4. The characters fill their cell completely, so that
          4 is also exactly what stands between the ink. The stroke is
          2 px thick at this scale, so it cannot go much lower without
          the letters sticking together. "NEDERLAND 2" is thereby wider
          than the ID box of 150 px; on the real card a shorter name is
          in there for that reason.
  PM8546  PROPORTIONAL on the ink width, digits on the width of the
          widest digit. Letter spacing 3. A running clock with a narrow
          1 would otherwise jump every second.

The PM8546 cannot go up any further than this: at 27 point the W is 25
columns wide and runs past MAX_COLS. So 26 point is the maximum.

LICENCE. From Nimbus we take only the RASTERS, not the font file
itself.
"""

import argparse
import io
import os
import re
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
MAX_ROWS = 44          # the PM8546 at size is 43 rows
MAX_COLS = 24          # widest character is the PM8546 'M' and 'W'

CHARS = " !\"&'()+,-./0123456789:;=?ABCDEFGHIJKLMNOPQRSTUVWXYZ_\u00d8"


def from_existing_table(path, target=None):
    """Pull the hand drawn 7x15 font out of the source table.

    With `target` every character is brought to that pixel size. That
    is (14, 30): scale 2 in both directions, so pure doubling and
    exactly the picture the firmware has had all along.
    """
    src = io.open(path, encoding="utf-8").read()
    out = []
    # The O with a slash through it sits in the source table as
    # GLYPH_OSLASH and not as a character between quotes, so it has to
    # be recognised separately, otherwise it silently drops out of the
    # font.
    for m in re.finditer(r"\{(GLYPH_OSLASH|'.'),\s*\{([^}]*)\}\}", src):
        c = chr(1) if m.group(1) == "GLYPH_OSLASH" else m.group(1)[1]
        rows = [int(v, 0) for v in m.group(2).split(",")]
        bits = np.zeros((len(rows), 7), np.uint8)
        for r, v in enumerate(rows):
            for k in range(7):
                bits[r, k] = (v >> (6 - k)) & 1
        if target:
            from PIL import Image
            im = Image.fromarray((bits * 255).astype(np.uint8)).resize(target, Image.NEAREST)
            bits = (np.asarray(im) > 128).astype(np.uint8)
        out.append((c, bits))
    return out


def from_prom(dir_path):
    """The PM8546 character set, exactly as it sits in the PROM.

    ONE BYTE IS ONE FRAME LINE, not a field line.

    So NOTHING has to be scaled: 33 rows fits in a text box of 43, and
    the characters stay pixel exact.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from extract_pm8546_font import read_table, read_glyph
    prom = np.fromfile(os.path.join(dir_path, "V18", "OpenPM8546_G.bin"), np.uint8)
    table = read_table(os.path.join(dir_path, "V5", "logogen.c"))
    return [(c, read_glyph(prom, address, length)) for c, length, address in table]


def from_otf(path, points, chars=None):
    """Render the characters at this point size and crop to the ink band.

    `chars` is there for a tool that needs another set: the display font
    in make_display_font.py also wants lower case, which the test cards
    have no use for. It defaults to CHARS, so the two fonts in
    font_data.h come out exactly as they did.
    """
    from PIL import Image, ImageDraw, ImageFont
    chars = CHARS if chars is None else chars
    f = ImageFont.truetype(path, points)
    # common baseline: draw everything on the same canvas and crop it
    # all in one go, otherwise the characters start dancing
    raw = {}
    for c in chars:
        im = Image.new("L", (MAX_COLS * 3, MAX_ROWS * 3), 0)
        ImageDraw.Draw(im).text((4, 2), c, font=f, fill=255)
        raw[c] = np.asarray(im) > 128
    rows = np.zeros(MAX_ROWS * 3, bool)
    for m in raw.values():
        rows |= m.any(1)
    on = np.flatnonzero(rows)
    y0, y1 = int(on.min()), int(on.max())
    # Within the firmware the O with a slash through it gets its own
    # one byte code; out of the font it just comes as a character.
    return [(chr(1) if c == "\u00d8" else c, raw[c][y0:y1 + 1])
            for c in chars]


def cap_reference(path, points, chars=None):
    """Where a plain flat capital (H) sits inside from_otf()'s own
    shared crop window, and how tall it is.

    Fixes a vertical alignment fault seen on the board on PM8546 and
    Inter in the ticker: ordinary text sat close to the top of the band
    with a lot of empty room below it. The cause: from_otf()'s crop
    window is the union of EVERY character, including outliers like the
    parentheses,
    which on these faces reach further above and below an ordinary
    capital than an ordinary capital reaches itself; ticker.cpp used to
    centre its padding on that whole outlier-inclusive window, so
    ordinary text (which essentially never uses those outliers) landed
    off centre by the same amount the outliers stick out.

    This re-renders the same characters (a second pass, at build time
    only, so the cost does not matter) rather than changing what
    from_otf() itself returns, to keep that function's own, working
    output untouched. Mirrors the same "H" reference
    tools/make_contest_font.py's caps_band() already uses for exactly
    this purpose on the run-glyph faces.
    """
    from PIL import Image, ImageDraw, ImageFont
    chars = CHARS if chars is None else chars
    f = ImageFont.truetype(path, points)

    def mask(c):
        im = Image.new("L", (MAX_COLS * 3, MAX_ROWS * 3), 0)
        ImageDraw.Draw(im).text((4, 2), c, font=f, fill=255)
        return np.asarray(im) > 128

    rows = np.zeros(MAX_ROWS * 3, bool)
    for c in chars:
        rows |= mask(c).any(1)
    y0 = int(np.flatnonzero(rows).min())
    on = np.flatnonzero(mask("H").any(1))
    return int(on.min()) - y0, int(on.max() - on.min() + 1)


NARROW = "-:.,!()"    # punctuation that fills only the first 3 source columns


def fixed_width(glyphs, wide, narrow):
    """Every character the full cell, only the punctuation narrower.

    That is how it is in the real PM5544: a block font in which every
    letter and every digit is equally wide. Only around the punctuation
    a gap would otherwise fall, so those get less.

    The rasters are NOT cropped here, they stay on the left in their
    cell, just as in the source table.
    """
    return [(c, m.astype(np.uint8), narrow if c in NARROW else wide) for c, m in glyphs]


def normalise(glyphs, space):
    """Crop every character to its ink width.

    The space has no ink and therefore no width, so here it gets a
    given value.
    """
    out = []
    for c, m in glyphs:
        k = np.flatnonzero(m.any(0))
        if len(k) == 0:
            out.append((c, np.zeros((m.shape[0], 1), np.uint8), space))
        else:
            out.append((c, m[:, k.min():k.max() + 1].astype(np.uint8), k.max() - k.min() + 1))
    return out


def digit_width(glyphs):
    """The widest of the ten digits.

    Digits all get that width. Letters may be proportional, but a
    running clock with a narrow 1 jumps every second and that shows
    straight away.
    """
    return max([b for c, m, b in glyphs if c.isdigit()] or [1])


def write_font(fh, name, glyphs, sx, sy, note):
    rows = glyphs[0][1].shape[0]
    assert rows <= MAX_ROWS, "%s is %d rows high, raise MAX_ROWS" % (name, rows)
    fh.write("// %s\n" % note)
    fh.write("static const Glyph FONT_%s_G[] = {\n" % name)
    for c, m, width in glyphs:
        assert width <= MAX_COLS, "%s: character %r is %d wide" % (name, c, width)
        words = []
        for r in range(rows):
            v = 0
            for k in range(m.shape[1]):
                if m[r, k]:
                    v |= 1 << (31 - k)
            words.append("0x%08X" % v)
        lit = "'\\''" if c == "'" else ("'\\\\'" if c == "\\" else "'%s'" % c)
        if c == "\x01":
            lit = "GLYPH_OSLASH"
        fh.write("    {%s, %2d, {%s}},\n" % (lit, width, ", ".join(words)))
    fh.write("};\n\n")
    return rows, digit_width(glyphs)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--prom", default=None, help="OpenPM8546 folder from PTV_Preservation")
    p.add_argument("--nimbus", default=None, help="NimbusSanL-Bol.otf")
    p.add_argument("--inter", default=None, help="Inter-Bold.ttf")
    p.add_argument("--square", default=None, help="Doto.ttf")
    a = p.parse_args()

    fonts = []
    # capTop/capRows: where a plain flat capital (H) sits within the
    # shared, outlier-inclusive cell, and how tall it is -- see
    # cap_reference()'s own note for why this exists. PM5544 is a hand
    # table, not cropped from a rendered font, so it has no outlier
    # problem to correct for: capTop 0, capRows its own fixed height.
    fonts.append(("PM5544", fixed_width(from_existing_table(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "font_pm5544.h"), (14, 30)),
        14, 6), 1, 1, 4,
        "Traced from a broadcast PM5544, 7x15 at scale 2. Fixed width.",
        0, 30))
    if a.nimbus:
        cap_top, cap_rows = cap_reference(a.nimbus, 26)
        fonts.append(("PM8546", normalise(from_otf(a.nimbus, 26), 8), 1, 1, 3,
                      "Nimbus Sans L Bold: close to the PM8546, which is why it is named after it.",
                      cap_top, cap_rows))
    if a.inter:
        # THE POINT SIZE IS SEARCHED, not chosen: a glyph row is one 32
        # bit word and the ring building of the ticker shifts it by the
        # edge width, so a character may be MAX_COLS wide at most. The
        # largest size whose widest character still fits is taken. The
        # BOLD weight, because these rasters are one bit deep: at this
        # size the regular strokes threshold to one or two uneven
        # pixels, and the PM8546 next to it is a bold face as well.
        best = None
        for points in range(20, 40):
            gl = normalise(from_otf(a.inter, points), 8)
            rows = gl[0][1].shape[0]
            wide = max(b for _, _, b in gl)
            if wide <= MAX_COLS and rows <= MAX_ROWS:
                best = (points, gl)
        points, gl = best
        cap_top, cap_rows = cap_reference(a.inter, points)
        fonts.append(("INTER", gl, 1, 1, 3,
                      "Inter Bold at %d point: the face of the contest page." % points,
                      cap_top, cap_rows))
    if a.square:
        # Picdream's own first-line face (see picdream.cpp), added here
        # too: tickerFont()/tickerSetFont() are just an index into
        # FONTS[] (ticker.h), and card text cycles the same array; there
        # has never been a separate, ticker-only list, so a fourth entry
        # here is what makes it selectable there and everywhere else a
        # font is cycled.
        #
        # SAME SIZE SEARCH AS INTER: the largest point size whose
        # widest character still fits MAX_COLS, since a glyph row is one
        # 32 bit word and the ticker's own ring shifts it further by the
        # edge width. Only one weight exists for this face, unlike
        # Inter, which needed Bold specifically because a regular weight
        # thresholds unevenly at one bit deep; a dot-matrix face is
        # blocky by design and does not have that problem.
        #
        # DOTO (SIL OFL 1.1, https://github.com/oliverlalan/Doto),
        # replacing Square-Dot-Matrix (Krafti Lab, "Free for Personal
        # Use"): that face could not be redistributed through this
        # project's public repository. See make_picdream_font.py's own
        # docstring.
        best = None
        for points in range(20, 40):
            gl = normalise(from_otf(a.square, points), 8)
            rows = gl[0][1].shape[0]
            wide = max(b for _, _, b in gl)
            if wide <= MAX_COLS and rows <= MAX_ROWS:
                best = (points, gl)
        points, gl = best
        cap_top, cap_rows = cap_reference(a.square, points)
        fonts.append(("SQUARE", gl, 1, 1, 3,
                      "Doto at %d point, a square-dot-matrix face for "
                      "Picdream's first line and the ticker." % points,
                      cap_top, cap_rows))

    nl = chr(10)
    with io.open(os.path.join(SRC, "font_data.h"), "w", encoding="utf-8", newline=nl) as fh:
        fh.write("""// GENERATED by tools/make_fonts.py. Do not edit by hand.
//
// The fonts. Every character is a row of 32 bit words, bit 31 being the
// leftmost column, plus its width in font columns and, per font, the
// letter spacing.
//
// PM5544 is FIXED width like the real generator: every letter and every
// digit 14 px, only -:.,!() get 6. PM8546 is PROPORTIONAL on the ink
// width, except for the digits, which get the width of the widest digit;
// otherwise a running clock with a narrow 1 would jump every second and
// that shows straight away.
//
// WHERE THEY COME FROM AND WHAT MAY BE USED
//   PM5544     traced from a photograph of a broadcast
//   PM8546     Nimbus Sans L Bold, only the rasters were taken and not
//              the font file itself. The real PM8546 character set has
//              been pulled out of the PROM, see extract_pm8546_font.py,
//              but it is not in here.
//   Inter      Inter Bold (SIL OFL 1.1, the TTF sits in tools/), the
//              same face the contest page carries.
//   Doto       square-dot-matrix face (SIL OFL 1.1, the TTF sits in
//              tools/).
#pragma once

#include <stdint.h>

// Internal one byte code for the O with a slash through it: in UTF-8
// that character is two bytes and so does not fit in a char literal.
#define GLYPH_OSLASH '\\x01'

""")
        fh.write("static const int FONT_MAX_ROWS = %d;%s%s" % (MAX_ROWS, nl, nl))
        fh.write("struct Glyph {" + nl)
        fh.write("  char c;" + nl)
        fh.write("  uint8_t cols;               // character width in font columns" + nl)
        fh.write("  uint32_t r[FONT_MAX_ROWS];  // bit 31 = leftmost column" + nl)
        fh.write("};" + nl + nl)
        fh.write("struct Font {" + nl)
        fh.write("  const char *name;" + nl)
        fh.write("  const Glyph *g;" + nl)
        fh.write("  uint16_t count;" + nl)
        fh.write("  uint8_t rows, sx, sy;" + nl)
        fh.write("  uint8_t digits;             // fixed width for 0-9" + nl)
        fh.write("  uint8_t gap;                // letter spacing when it fits" + nl)
        fh.write("  // Where a plain flat capital (H) sits within rows/0..rows-1,\n"
                  "  // and how tall it is: rows itself is the outlier-inclusive\n"
                  "  // cell (parentheses and the like can reach further than an\n"
                  "  // ordinary capital), so the ticker centres its padding on\n"
                  "  // capTop/capRows instead, or ordinary text lands off centre\n"
                  "  // by however far the outliers reach. See cap_reference() in\n"
                  "  // make_fonts.py.\n"
                  "  uint8_t capTop, capRows;" + nl)
        fh.write("};" + nl + nl)

        sizes = []
        for name, glyphs, sx, sy, gap, note, capTop, capRows in fonts:
            rows, digit = write_font(fh, name, glyphs, sx, sy, note)
            sizes.append((name, len(glyphs), rows, sx, sy, digit, gap, capTop, capRows))

        # The table symbol has to be a C identifier, the shown name does
        # not; only the Inter entry differs between the two.
        display = {"INTER": "Inter", "SQUARE": "Doto"}
        fh.write("static const Font FONTS[] = {" + nl)
        for name, n, rows, sx, sy, digit, gap, capTop, capRows in sizes:
            fh.write("    {\"%s\", FONT_%s_G, %d, %d, %d, %d, %d, %d, %d, %d},%s"
                     % (display.get(name, name), name, n, rows, sx, sy, digit, gap, capTop,
                        capRows, nl))
        fh.write("};" + nl)
        fh.write("static const int FONT_COUNT = %d;%s" % (len(sizes), nl))

    for name, n, rows, sx, sy, digit, gap, capTop, capRows in sizes:
        print("  %-10s %2d characters, %2d rows, scale %dx%d, digit width %2d, spacing %d, "
              "cap %d+%d"
              % (name, n, rows, sx, sy, digit, gap, capTop, capRows))
    print("src/font_data.h written")
    return 0


if __name__ == "__main__":
    sys.exit(main())

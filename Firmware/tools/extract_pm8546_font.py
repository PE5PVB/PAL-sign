"""
Pulls the font of the Philips PM8546 logo generator out of the PROMs.

    python tools/extract_pm8546_font.py <folder-with-OpenPM8546>

The PM8546 is the logo generator that fills the text boxes of the PM5644.
Its character set sits in the character generator PROMs, and those have
been preserved, together with a reconstruction of the firmware, in
github.com/inaxeon/PTV_Preservation under PM5644/PM8546/OpenPM8546.

Two things out of that folder are needed:
  V18/OpenPM8546_G.bin   the PROM with the character pictures (65536 bytes)
  V5/logogen.c           in there sits _g_char_blocks[], the table that
                         says per character where it is in the PROM

HOW THE PROM IS PUT TOGETHER, worked out by measuring and not from a
description, since there is none.

The table gives { length, address } per character. The address is a
BLOCK number and a block is 128 bytes; character 'A' sits at block 0x00,
'B' at 0x02, and the comment in the source code names those as byte
address 0 and 256. So a block is 128 bytes and the length is the number
of blocks.

Within a block every byte is ONE PICTURE LINE of eight pixels, and
successive blocks are successive columns of eight pixels. So a character
of two blocks is 16 px wide.

Every block holds the character TWICE: once on row 5..37 and once more
on row 69..101, that second copy shifted a few pixels to the left. That
is the half cell shift with which the hardware can put characters at
half positions. Here the first copy is taken.

A byte is a FRAME line and not a field line.

Yields 76 characters of 33 rows high and 8, 16 or 24 px wide, so
lowercase letters as well, which the current traced 7x15 font does not
have.
"""

import argparse
import os
import re
import sys

import numpy as np

BLOCK = 128         # bytes per block, that is picture lines per column
CELL_Y0, CELL_Y1 = 5, 37


def read_table(path):
    """The table { length, block address } per character out of logogen.c."""
    src = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"_g_char_blocks\[\] = \{(.*?)\n\};", src, re.S)
    if not m:
        raise SystemExit("_g_char_blocks not found in %s" % path)
    out = []
    for e in re.finditer(r"\{\s*(\d+),\s*(0x[0-9A-Fa-f]+)\s*\},\s*//\s*(.*)", m.group(1)):
        length, address, cmt = int(e.group(1)), int(e.group(2), 16), e.group(3)
        char = re.search(r"'(.)'", cmt)
        # length 0 is "NOT ALLOWED", and anything above 3 blocks is a logo
        if length == 0 or length > 3 or not char:
            continue
        out.append((char.group(1), length, address))
    return out


def read_glyph(prom, address, length):
    columns = [np.unpackbits(prom[(address + i) * BLOCK:(address + i + 1) * BLOCK].reshape(-1, 1),
                             axis=1) for i in range(length)]
    return np.concatenate(columns, axis=1)[CELL_Y0:CELL_Y1 + 1]


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("directory", help="the OpenPM8546 folder from PTV_Preservation")
    p.add_argument("--png", help="write an overview of the character set")
    a = p.parse_args()

    prom_path = os.path.join(a.directory, "V18", "OpenPM8546_G.bin")
    src_path = os.path.join(a.directory, "V5", "logogen.c")
    for q in (prom_path, src_path):
        if not os.path.exists(q):
            raise SystemExit("not found: %s" % q)

    prom = np.fromfile(prom_path, np.uint8)
    if prom.size != 65536:
        raise SystemExit("PROM is %d bytes, expected 65536" % prom.size)

    table = read_table(src_path)
    glyphs = [(c, read_glyph(prom, address, length)) for c, length, address in table]
    widths = sorted(set(g.shape[1] for _, g in glyphs))
    print("%d characters, %d rows high, widths %s"
          % (len(glyphs), glyphs[0][1].shape[0], widths))
    print("character set: %s" % "".join(c for c, _ in glyphs))

    if a.png:
        from PIL import Image
        cols = 16
        rows = (len(glyphs) + cols - 1) // cols
        cw, ch = max(widths) + 4, (CELL_Y1 - CELL_Y0 + 1) + 4
        canvas = np.zeros((rows * ch, cols * cw), np.uint8)
        for i, (c, g) in enumerate(glyphs):
            r, k = divmod(i, cols)
            canvas[r * ch + 2:r * ch + 2 + g.shape[0], k * cw + 2:k * cw + 2 + g.shape[1]] = g * 255
        # One to one: a byte is a FRAME line.
        Image.fromarray(255 - canvas).resize((cols * cw * 2, rows * ch * 2), Image.NEAREST).save(a.png)
        print("overview written: %s" % a.png)
    return 0


if __name__ == "__main__":
    sys.exit(main())

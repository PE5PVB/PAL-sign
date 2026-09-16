"""
Decodes a built in test card from the flash data and writes pictures of
it, so that you can check it on the PC.

    python tools/show_testcard.py testcardf --reference reference.jpg
    python tools/show_testcard.py testcardw --ratio 16:9
    python tools/show_testcard.py pm5644g924 --variant 2
    python tools/show_testcard.py --list

  <name>_picture.png  the raster picture of 720x576 as it is in flash
  <name>_screen.png   the same, stretched to the SCREEN ratio
  <name>_overlay.png  the difference with the reference, coloured red
                      (only with --reference)

That second one is important: 720x576 is anamorphic. A picture pixel is
at 4:3 1.07 times as wide as it is high and at 16:9 even 1.42 times. On
a PC the pixels are shown square and everything looks horizontally
squeezed, the slanted hatching lines then look 55 degrees while on a
16:9 screen they are neatly 45 degrees. So for angles and curves look at
<name>_screen.png and not at the raster picture.

TWO THINGS ARE NOT THE SAME FOR EVERY CARD. Only Test Card F and Test
Card W carry a separate photograph, in <name>_photo.inc.h next to the
tables; the rest is entirely run length coded. And a card can hold more
than one ROW INDEX, one per variant: the G00 has three (the insert
boxes), the G924 four and Test Card G two. --variant picks between them
and --list says how many there are.
"""

import argparse
import glob
import io
import os
import re
import sys

import numpy as np

from asmtable import read_table
from PIL import Image

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "testcards")

W, H = 720, 576
ROW_WORDS = W // 2  # one word is a pixel pair in 4:2:2


def cards():
    """Every card in src/testcards, with its variant count and photo."""
    out = []
    for path in sorted(glob.glob(os.path.join(SRC, "*_line.inc.h"))):
        name = os.path.basename(path)[:-len("_line.inc.h")]
        variants = len(read_table(path)) // (H * 2)
        photo = os.path.exists(os.path.join(SRC, name + "_photo.inc.h"))
        out.append((name, variants, photo))
    return out


def paste_photo(name, frame):
    """Draws the separate photograph over the decoded tables.

    Only Test Card F and Test Card W have one. The rows are stored as
    spans: a start, a length and an offset per picture row, all of them
    in pixel PAIRS, because a 4:2:2 pair may not be cut in half.
    """
    p = lambda e: os.path.join(SRC, name + e)
    if not os.path.exists(p("_photo.inc.h")):
        return False

    photo = np.frombuffer(read_table(p("_photo.inc.h")), np.uint8)
    h = io.open(p("_photo.h"), encoding="utf-8").read()
    y0 = int(re.search(r"_PHOTO_Y0 = (\d+)", h).group(1))
    rows = int(re.search(r"_PHOTO_ROWS = (\d+)", h).group(1))

    def array(field):
        m = re.search(name + "_photo_" + field + r"\[[^\]]*\] = \{(.*?)\};", h, re.S)
        return [int(v) for v in m.group(1).replace(chr(10), "").split(",") if v.strip()]

    x0s, lens, offs = array("x0"), array("len"), array("off")
    for r in range(rows):
        y = y0 + r
        assert x0s[r] % 2 == 0 and lens[r] % 2 == 0
        frame[y, x0s[r] * 2:x0s[r] * 2 + lens[r] * 2] = photo[offs[r]:offs[r] + lens[r] * 2]
    return True


def decode(name, variant=0):
    """The picture as 576 rows of 1440 bytes, exactly as flash holds it.

    This walks the run lengths the same way romRenderFrom() in the
    firmware does, so what comes out here is what the board puts out.
    """
    p = lambda e: os.path.join(SRC, name + e)
    if not os.path.exists(p("_line.inc.h")):
        raise SystemExit("no card called %s in src/testcards; --list says what there is" % name)

    pal = np.frombuffer(read_table(p("_pal.inc.h")), "<u4")
    idx = np.frombuffer(read_table(p("_idx.inc.h")), "<u2")
    ln = np.frombuffer(read_table(p("_len.inc.h")), np.uint8)
    start = np.frombuffer(read_table(p("_start.inc.h")), "<u4")
    line = np.frombuffer(read_table(p("_line.inc.h")), "<u2")

    variants = len(line) // H
    if not 0 <= variant < variants:
        raise SystemExit("%s has %d variant(s), so --variant must be 0..%d"
                         % (name, variants, variants - 1))
    line = line[variant * H:(variant + 1) * H]

    frame = np.zeros((H, W * 2), np.uint8)
    for y in range(H):
        li = line[y]
        words = []
        for k in range(start[li], start[li + 1]):
            words += [pal[idx[k]]] * ln[k]
        assert len(words) == ROW_WORDS, "%s: row %d has %d words" % (name, y, len(words))
        frame[y] = np.array(words, "<u4").view(np.uint8)
    return frame


def to_rgb(fr):
    Y = fr[:, 1::2].astype(np.float64)
    CB = np.repeat(fr[:, 0::4], 2, 1).astype(np.float64)
    CR = np.repeat(fr[:, 2::4], 2, 1).astype(np.float64)
    y = (Y - 16) / 219.0
    cb = (CB - 128) / 112.0 * 0.886
    cr = (CR - 128) / 112.0 * 0.701
    R = y + cr
    B = y + cb
    G = (y - 0.299 * R - 0.114 * B) / 0.587
    return np.clip(np.stack([R, G, B], -1) * 255, 0, 255).astype(np.uint8)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("name", nargs="?", help="the card, so testcardf, pm5644g00 and so on")
    p.add_argument("--ratio", default="4:3", help="screen ratio for _screen.png (default 4:3)")
    p.add_argument("--variant", type=int, default=0, help="which row index (default 0)")
    p.add_argument("--reference", help="compare with this picture and write _overlay.png")
    p.add_argument("--out", default=os.path.expanduser("~/Downloads"),
                   help="where the pictures go (default ~/Downloads)")
    p.add_argument("--list", action="store_true", help="show every card and stop")
    a = p.parse_args()

    if a.list or not a.name:
        print("%-14s %8s  %s" % ("card", "variants", "photo"))
        for name, variants, photo in cards():
            print("%-14s %8d  %s" % (name, variants, "yes" if photo else "no"))
        return 0 if a.list else 2

    frame = decode(a.name, a.variant)
    had_photo = paste_photo(a.name, frame)
    rgb = to_rgb(frame)
    Image.fromarray(rgb).save("%s/%s_picture.png" % (a.out, a.name))

    num, den = (int(v) for v in a.ratio.split(":"))
    width = int(round(float(H) * num / den))
    Image.fromarray(rgb).resize((width, H), Image.LANCZOS).save(
        "%s/%s_screen.png" % (a.out, a.name))
    print("%s: variant %d%s" % (a.name, a.variant, ", photograph pasted in" if had_photo else ""))
    print("   %s_picture.png (raster) and %s_screen.png (%s, %dx%d)"
          % (a.name, a.name, a.ratio, width, H))

    if a.reference:
        ref = Image.open(a.reference).convert("RGB")
        if ref.size != (W, H):
            ref = ref.resize((W, H), Image.LANCZOS)
        r = np.asarray(ref).astype(np.float64)
        d = np.abs(r.mean(2) - rgb.mean(2)) > 70
        layer = (rgb * 0.45).astype(np.uint8)
        layer[d] = (255, 0, 0)
        Image.fromarray(layer).resize((width, H), Image.LANCZOS).save(
            "%s/%s_overlay.png" % (a.out, a.name))
        print("   %s_overlay.png: %d pixels differ (%.2f percent)"
              % (a.name, int(d.sum()), 100.0 * d.mean()))
    return 0


if __name__ == "__main__":
    sys.exit(main())

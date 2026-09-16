# -*- coding: utf-8 -*-
"""EBU colour bars over a red field, drawn from the specification.

WHAT IT IS. Eight equal colour bars over the top two thirds of the
picture and a plain red field under them. A slate: the bars let you check
the colour decoder and the red field shows up ringing, noise and
compression on a large flat area of one colour.

Nothing is taken from a reference. The colours come from the standard and
the geometry is the geometry of the raster; the reference was used to
measure where the horizontal boundary lies and to confirm which of the
two bar amplitudes it is.

THE COLOURS are the 75 percent colour bars of ITU-R BT.471-1, with the
white bar at 100 percent. So the six colour bars and the white are:

    white   R 255  G 255  B 255      cyan    R   0  G 191  B 191
    yellow  R 191  G 191  B   0      green   R   0  G 191  B   0
    magenta R 191  G   0  B 191      red     R 191  G   0  B   0
    blue    R   0  G   0  B 191      black   R   0  G   0  B   0

191 is 75 percent of 255, rounded. The order is the standard one: from
the left in falling luminance.

EIGHT EQUAL BARS over 720 active samples is 90 samples each, so every
boundary falls on a whole sample and no bar is a fraction wider than its
neighbour. That also puts every boundary on an even sample, which
matters in 4:2:2: a pixel PAIR shares one Cb and one Cr, so a boundary on
an odd sample would give a pair with two different colours in it and the
chroma would have to be an average of the two.

INTERLACE. Even picture rows go to field 1 and odd ones to field 2 (see
video.cpp). A horizontal band with an odd number of rows therefore comes
out one row thicker in one field than in the other and flickers at 25 Hz.
The boundary is on row 372, so the bars are 372 rows and the red field
204, both even.

TWO UNIQUE PICTURE ROWS is all this card has, so it costs almost nothing
in flash and nothing at all in RAM beyond the shared working copy in
rompattern.cpp.
"""
import argparse
import sys

import numpy as np
from PIL import Image

from convert_rom_pattern import encode_and_write
from make_testcardf import to_422

W, H = 720, 576

# ITU-R BT.471-1, the 75 percent bars, with white at 100 percent. In the
# standard order, so falling in luminance from the left.
BARS = [
    (255, 255, 255),  # white
    (191, 191, 0),    # yellow
    (0, 191, 191),    # cyan
    (0, 191, 0),      # green
    (191, 0, 191),    # magenta
    (191, 0, 0),      # red
    (0, 0, 191),      # blue
    (0, 0, 0),        # black
]
BAR_W = W // len(BARS)

# The row where the bars stop and the red field starts. Measured in the
# reference at half amplitude in three separate groups of columns, all
# three within 0.4 of a row of each other. The red field is the same red
# as the sixth bar, which was measured as well.
SPLIT = 372
RED = BARS[5]


def draw_card():
    """The whole picture as RGB, 576 rows of 720 pixels."""
    card = np.zeros((H, W, 3), np.float64)
    for i, rgb in enumerate(BARS):
        card[0:SPLIT, i * BAR_W:(i + 1) * BAR_W] = rgb
    card[SPLIT:H] = RED
    return card


def even_boundaries():
    """Fails if a band would come out with an odd number of rows.

    See the note about interlace in the module docstring. This is a
    build-time check and not a comment, so that changing SPLIT to an odd
    row cannot slip through.
    """
    for name, n in (("bars", SPLIT), ("red field", H - SPLIT)):
        if n % 2:
            raise SystemExit("%s is %d rows, which is odd: it would flicker at 25 Hz"
                             % (name, n))


def even_bars():
    """Fails if a bar boundary would fall on an odd sample.

    See the note about 4:2:2 in the module docstring.
    """
    if W % (2 * len(BARS)):
        raise SystemExit("%d samples over %d bars does not give an even boundary per bar"
                         % (W, len(BARS)))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--png", help="write the result as a PNG")
    a = p.parse_args()

    even_boundaries()
    even_bars()

    card = draw_card()
    print("card drawn")
    print("  %d bars of %d samples, row 0..%d" % (len(BARS), BAR_W, SPLIT - 1))
    print("  red field row %d..%d, R%d G%d B%d" % ((SPLIT, H - 1) + RED))

    if a.png:
        Image.fromarray(np.clip(card, 0, 255).astype(np.uint8)).save(a.png)
        print("  %s written" % a.png)

    cfg = {"title": "EBU bars and red",
           "note": "fully drawn; BT.471 75 percent bars, white at 100 percent"}
    encode_and_write("barsred", cfg, [to_422(card)], 0)


if __name__ == "__main__":
    sys.exit(main())

"""
Draws BBC Test Card F, and takes only the photograph from a reference.

    python tools/make_testcardf.py c:/path/tcf.jpg --png compare.png

Delivers everything the test card needs:
  src/testcards/testcardf_{pal,idx,len,start,line}.bin + _data.h + _data.S  (the card)
  src/testcards/testcardf_photo.inc.h + _photo.S + _photo.h                   (the photo)

WHAT IS DRAWN AND WHAT IS NOT. Everything except the photograph in the
centre circle is calculated: frame, colour bars, castellations, side
bars, grid, corner blocks, greyscale staircase, frequency gratings,
needle block, the letter F, the crosses, the overscan triangles and the
circle ring. The photograph cannot be drawn and comes straight from the
reference.

SLANTED AND ROUND EDGES are supersampled; straight edges are not. See
the "antialiasing" block halfway down this file for what that costs and
why it has to be selective.

WHY IT IS DRAWN. Taking it over unprocessed cost 810 kB and gave the
JPEG approximation of the source: areas that wobble, edges that run over
three pixels, and measured values that are wrong. Drawn, the areas
really are flat, the edges hard, and the measurement elements sit at
their prescribed value.

THE MEASUREMENT VALUES, and where they come from:

  gratings    1.5 / 2.5 / 3.5 / 4 / 4.5 / 5.25 MHz. Published, see
              en.wikipedia.org/wiki/Test_Card_F. In the reference those
              frequencies are right (FFT: 1.49 2.49 3.49 3.98 4.48 5.23)
              but the amplitude drops from 60 to 14 over the six bands,
              and then the roll off of the chain cannot be separated
              from that of the source. Here all six sit at the same
              amplitude.

  colour bars 95 percent saturation, published as well; Test Card W went
              to 100 percent later. The reference does not reach that:
              measured it sits at 67 to 86 percent. Here the 95 percent
              is calculated.

  greyscale   six steps. NO specification has been published for this,
              so these values come from the reference itself, measured
              in flat interior areas of column 185.

The GEOMETRY is measured in the reference as well: for Test Card F there
is no drawable specification. It started life as a photographed physical
card; the bbc-testcards project says it had to redraw most of it by
hand. Behind every dimension below is where it comes from.
"""

import argparse
import io
import os
import sys

import numpy as np

from asmtable import write_table
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_rom_pattern import encode_and_write

W, H = 720, 576
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src",
                       "testcards")

WHITE = (255.0, 255.0, 255.0)
BLACK = (0.0, 0.0, 0.0)
GREY = (126.0, 126.0, 126.0)          # the field, measured on row 300


def saturate(full, s=0.95):
    """Bring a fully saturated colour back to s saturation.

    Mix with grey of the SAME brightness, so that only the colour
    subcarrier gets smaller and the luminance step of the bars is left
    alone: that is what saturation means.
    """
    y = 0.299 * full[0] + 0.587 * full[1] + 0.114 * full[2]
    return tuple(y + s * (v - y) for v in full)


YELLOW = saturate((255, 255, 0))
CYAN = saturate((0, 255, 255))
GREEN = saturate((0, 255, 0))
MAGENTA = saturate((255, 0, 255))
RED = saturate((255, 0, 0))
BLUE = saturate((0, 0, 255))

# --- geometry, all measured in the reference ----------------------------
# Top colour bar: column 400 gives the height (transition at 28.5, so
# row 0 up to and including 28). The eight boundaries were measured per
# colour channel separately and averaged over rows 8 to 20: 52.56 140.37
# 228.28 316.25 403.84 491.68 579.52 667.26. Rounded to the nearest
# pixel boundary that gives the series below.
BAR_H = 29
BAR_X = (0, 53, 141, 229, 317, 404, 492, 580, 668, 720)
BAR_C = (WHITE, YELLOW, CYAN, GREEN, MAGENTA, RED, BLUE, BLACK, WHITE)

# Frame. Measured at half height, over rows 245..334 (where the side bar
# is black) and columns 585..614:
#   left    outer edge 35.55  inner edge 40.61
#   right   inner edge 678.86 outer edge 683.70
#   top     outer edge 28.51  inner edge 33.66
#   bottom  inner edge 540.4  outer edge 545.6
# So in the source the frame is a good five pixels thick and not six,
# but six it stays: the top and bottom bar are horizontal bands and
# those have to be an even number of rows high (see even_band).
FRAME_L, FRAME_R, FRAME_T, FRAME_B = 35, 684, 29, 545
FRAME_D = 6

# Side bars, measured in columns 8, 12 and 16 and 704, 708 and 712.
#
# They TOUCH THE FRAME. In the source there is no grey between the side
# bar and the frame: on row 250 and row 330 the black of the bar runs on
# to 35.2 and the white starts right there. The same at the top: at
# 28.52 the white colour bar goes straight over into the black of the
# side bar.
SIDE_L, SIDE_R = 35, 685
# Measured at half height, the transitions sit at 85.52 132.53 238.51
# 440.48 487.51 535.53 on the left and 55.48 94.56 180.50 238.52 339.50
# 392.50 478.47 516.54 on the right. The transition to the bottom blue
# on the left sits at 333.59. At the bottom the black stops at 554.5,
# where the white end block of the castellation begins.
SIDE_LEFT = ((29, 36, BLACK), (37, 85, RED), (86, 132, BLACK), (133, 238, RED),
             (239, 333, BLACK), (334, 440, BLUE), (441, 487, BLACK), (488, 535, BLUE),
             (536, 554, BLACK))
SIDE_RIGHT = ((29, 55, BLACK), (56, 94, YELLOW), (95, 180, BLACK), (181, 238, WHITE),
              (239, 339, BLACK), (340, 392, WHITE), (393, 478, BLACK), (479, 516, YELLOW),
              (517, 554, BLACK))

# Bottom castellation. The transitions on row 552 sit at 53, 82, 111,
# 141, 170, 200, 228, 258, 287, 317 and on the other side at 404, 434,
# 462, 492, 521, 551, 580, 610, 638, 668. Pitch 29.33, but mind the
# CENTRE: between 317 and 404 there is a block three pitches wide and it
# is black. So the strip is built up from the centre and not from the
# left; with a continuous alternation the phase comes out wrong to the
# right of the centre.
# The strip starts at 545.6 and not at 547: measured in column 600 the
# white of the frame goes over into the green of the first tooth there.
CAST_Y0, CAST_Y1 = 546, 575
CAST_MID = (317.0, 404.0)
CAST_W = 29.33
# The ends. On row 560 the white on the left runs to 23.04 and on the
# right it starts at 696.52; those are the blocks 0..22 and 697..719.
# But they only start at 554.5: in column 10 and column 710 it is still
# black above that row, because the side bar simply runs on there. So
# above the white block belongs black and not a tooth: the last half
# tooth that the build up from the centre puts there does not exist in
# the source.
CAST_WHITE_Y0 = 555
CAST_END = ((0, 23), (697, W - 1))      # black over the whole strip
CAST_WHITE = ((0, 22), (697, W - 1))       # white from CAST_WHITE_Y0

# Grid. Measured vertically in the band y 430-445, horizontally in the
# column band x 44-150. The lines with black borders are the ones that
# bound the centre box.
# Measured vertically in the clean band y 100..140: lines at 155, 213.5,
# 330.5, 506 and 564.5. That is a pitch of 58.5 starting from 96.5.
RX0, RDX, RNX = 96.5, 58.5, 10
# Width, measured across the lines on row 120: a plain line is 6 px
# white, a black bordered one is 3 black, 3 white, 3 black.
R_WHITE_V = 6
# (thickness of ONE black border, width of the white). Measured across
# the line on row 120: black 3, white 3, black 3.
R_BORDERED_V = (3, 3)
# There are SEVEN horizontal lines, not five: at 95, 159, 223, 287, 351,
# 415 and 479, pitch 64. The line at 479 is the white line under the
# letter F.
RY0, RDY, RNY = 95.0, 64.0, 7
# FOUR black bordered verticals, not two. The "bars" beside the letter F
# turn out to be the black borders of the lines at x=330.5 and x=389:
# measured, the black at x=334 runs from y 34 to 131 and again from 442
# to 481, with the circle in between. So it is not a separate element
# but simply grid.
R_BLACK_X = (1, 4, 5, 8)
# Only the outer two run on to the frame; 4 and 5 stop at the grid line
# on 479, just like the plain lines.
R_BLACK_X_THROUGH = (1, 8)
# And of the PLAIN lines the outer two also run on to the frame. That is
# the first and the last, so exactly the ones in the corner blocks with
# the hatching.
#
# Measured on column 96 in Tcfm.jpg: under the hatching the white runs
# unbroken from row 517 to 544, where it meets the bottom horizontal
# line. On column 623 on the right the picture is the same.
#
# The hatching is drawn over it, so the stripes cut the line neatly,
# exactly as in the reference, where it is broken at 502..507 and
# 511..516.
R_PLAIN_X_THROUGH = (0, 9)
R_BLACK_Y = (1, 3, 5)      # only these three have black borders
# Thickness. Measured, the plain white line is 5 rows and the black
# bordered one 3+3+3. Both odd, and that flickers (see even_band). So
# they become 6 for the plain line and 4+2+4 for the bordered one.
R_WHITE = 6
# (thickness of ONE black border, height of the white). Measured on
# column 100: black 155..157, white 158..160, black 161..163. Four and
# two keeps both bands an even number of rows high, which interlace
# needs.
R_BORDERED = (4, 2)

# The centre box, which holds the photo circle, the greyscale staircase
# and the gratings, is bounded by four black bordered grid lines. On the
# INSIDE of those there is NO black border: there the white borders
# straight on to the field. Measured on the line y=159, column 230
# (inside) has black only above, while column 100 (outside the box) has
# black on both sides. The same for the other three edges.
MIDBOX_X = (1, 8)
MIDBOX_Y = (1, 5)

# The PLAIN vertical lines do not run on to the frame but stop at the
# bottom grid line on y=479: below that the card is one closed grey band
# over the full width. Measured: on row 484 nothing is left of the line
# at x=213.5, while the two black bordered lines do simply run on there.
R_STOP_Y = 6

# Corner hatching. A diagonal BAND with eight parallel black stripes in
# it, cut off at a slant at the FAR end, not at the corner of the grid
# box.
#
# Everything below is measured in the reference, per corner separately,
# with subpixel accuracy (centre of gravity of each stripe per picture
# row, about 375 stripe centres per corner).
#
# SLOPE. The stripes do NOT sit at 45 degrees in the raster. Measured,
# dx/dy = 0.9256 in all four corners (0.9253 0.9259 0.9254 0.9259), with
# a residual of 0.11 px on the straight line. At PAR 1.0667 that is
# 0.987 on the SCREEN, so 44.6 degrees.
CORNER_S = 0.9256
# PITCH and STRIPE WIDTH, measured along a picture row: pitch 8.826 px
# (8.826 8.826 8.822 8.829), stripe 4.42 px at half height between black
# and white (n=65, spread 0.08). That is 50 percent black.
CORNER_PITCH = 8.826
CORNER_WIDTH = 4.42
CORNER_N = 8
# BAND WIDTH. The white band is 86.9 px wide measured along a picture
# row (2 x 43.45; per corner 43.28 43.46 43.61 43.51). The eight stripes
# sit in it symmetrically: their centres fall on c +/- 0.5 1.5 2.5 3.5
# pitch, after which 10.3 px of white is left on both sides.
CORNER_HALF = 43.45
# The slanted CUT at the far end runs mirrored to the band, with the
# same slope but the other sign: measured -0.924 where the band is
# +0.9256. It sits at a fixed value of w = x +/- 0.9256*y (spread 0.06
# px over 37 to 40 rows). The BLACK stops 10.6 px before it, measured at
# half height over the six whole stripes of each corner (10.61 10.61
# 10.57 10.73), just as it stops 10.3 px before the side edges.
CORNER_EDGEW = 10.6
# Per corner: the box (within which only the corner itself still clips),
# the sign of the band direction, the centre of the BAND and that of the
# STRIPE pattern in u = x - sign*0.9256*y, the position of the cut in
# w = x + sign*0.9256*y, and on which side of that cut the band lies
# (+1 = w smaller). Band centre and stripe centre are measured
# separately, from the white edges and from the stripe centres, and
# differ by at most 0.37 px.
CORNER = (
    ((41, 150, 35, 153), +1,    9.64,    9.55,  232.13, +1),   # top left
    ((569, 678, 35, 153), -1,  709.76,  710.03,  487.42, -1),  # top right
    ((41, 150, 420, 539), -1,  540.84,  540.82, -299.22, +1),  # bottom left
    ((569, 678, 420, 539), +1, 178.99,  178.62, 1018.73, -1),  # bottom right
)

# Greyscale staircase: column 185. Six steps of 42 rows from y 159,
# x 161..209.
STAIR_X0, STAIR_X1 = 161, 209
STAIR_Y0, STAIR_STEP = 159, 42
STAIR_N = (236, 188, 138, 89, 46, 1)
# The two measurement patches in the staircase, measured at half height
# between the patch and the step around it:
#   peak white     x 179.9..188.1  y 180.6..187.3
#   lifted black   x 180.2..187.9  y 386.2..391.9
# The black patch is SEVEN rows high, and that is odd, exactly what
# even_band says is not allowed. It becomes six.
# The level of the black patch is 36 and not 34: average of the flat
# core (x 182..187, y 388..391) in the reference.
STAIR_PATCH_L = (180, 188, 182, 187, 255)    # peak white in the lightest step
STAIR_PATCH_D = (180, 188, 386, 391, 36)     # lifted black in the darkest

# Gratings: block x 503..570, band boundaries from the periodicity per
# row. The gratings belong exactly in the grey box between grid line 7
# and 8. That is calculated from the grid itself below instead of as
# fixed numbers, so that they cannot run over the lines. Measured, the
# modulation on row 180 ran from x 510 to 558, and that box is 509..559.
GRAT_CELL = 7
GRAT_BANDS = (164, 206, 247, 288, 329, 370, 410)
GRAT_MHZ = (1.5, 2.5, 3.5, 4.0, 4.5, 5.25)
GRAT_AMP = 100.0
FS = 13.5e6

# Needle block at the top: measured on row 100 and column 300.
NEEDLE = (259, 461, 67, 123)
NEEDLE_BAR = (296, 423, 82, 106)

# Crosses left and right of the centre, measured on y 253..319. They are
# NOT plus signs: they are two short vertical bars beside the outermost
# grid line. The horizontal leg you think you see is simply the black
# bordered grid line on y=287 running straight through them. Measured on
# rows 255, 265, 275, 300, 310 and 318: black at x 92..94 and 99..101 on
# the left, at 619..621 and 626..628 on the right, both over y 253..319.
CROSS_BARS = ((92, 94), (99, 101), (619, 621), (626, 628))
CROSS_Y = (253, 319)

# The bars beside the F, measured on row 450: four narrow black bars.
# Vertical bars do not flicker, so their height may be odd.
F_BARS = ((326, 328), (333, 335), (385, 386), (392, 393))
# At the top up into the circle: that is drawn afterwards and cuts the
# bars off exactly on the ring, so that they close up tight against it.
# At the bottom up to and including the grid line on 479.
F_BAR_Y = (425, 481)
# The letter F is NOT drawn but taken from the reference. A redrawn F
# deviated too much: it is a serif letter and that cannot be
# approximated with a few rectangles. This window lies between the inner
# bars, under the circle ring (which ends around y 443) and above the
# white line. The letter itself sits in it at y 448..473.
F_STAMP = (336, 384, 445, 475)
# The box runs from 443.5 to 479 with centre 461.3, while the letter
# itself sits at 448..473 with centre 460.5, so in the source the letter
# is already centred in it and no shift is needed.
F_UP = 0

# Photo circle. The OUTER edge of the white ring measured at half height
# between grey and white and over thirty columns instead of only through
# the centre:
#   widest row (y=278): left 210.43 right 508.41 -> cx 359.4 rx 149.0
#   calculating back per column to the axes      -> cy 287.0 ry 156.2
#   measured separately through the centre (column 359): top 129.3
#   bottom 443.5, so cy 286.4 and ry 157.1
# With ry/rx = 1.05 the circle is almost round on a 4:3 screen (PAR
# 1.0667 would ask for 1.0667).
CIR_CX, CIR_CY = 359.0, 286.5
CIR_RX, CIR_RY = 149.0, 156.5
CIR_RING = 9.0

# The photograph comes from a SEPARATE, sharper scan of 304x304 that
# shows more than the circle: the card only shows a crop of it. Where
# that crop sits is fitted: the scan is laid over a range of scales and
# shifts and the position that correlates best with what is inside the
# circle of the card source is the one that is used. The best reaches
# 0.85 correlation.
#
# The scale differs in x and y (1.180 against 1.260) because a pixel in
# the 720x576 raster is wider than it is tall while the scan has square
# pixels.
PHOTO_SCALE = (1.180, 1.260)
PHOTO_CORNER = (199.0, 115.0)

# Overscan triangles, in the middle of each edge. The POINT is on the
# outside and the BASE on the picture edge, so they point outwards.
# Measured at the top: on row 4 the white is 3 px wide, on row 28 it is
# 28 px; at the bottom from 28 px on row 546 to 2 px on row 572. On the
# left from nothing at x 10 to 27 px at x 33, on the right mirrored from
# x 686 to x 709. The base of the left and the right one moves one pixel
# with the frame (34 instead of 33, 685 instead of 686) so that it
# closes up against it; on row 20 the white in the source runs from
# 349.8 to 369.7 and here from 350.5 to 369.5.
TRI_LONG = 25
TRI_HALF = 14
TRI_BASE = (28, 546, 34, 685)     # top, bottom, left, right

# AND THERE IS ANOTHER SMALL TRIANGLE on the left and the right edge,
# with its BASE on the picture edge and its point INWARDS, so against
# the large triangle.
#
# Measured on the left in Tcfm.jpg, the WHITE piece that is attached to
# x=0:
#   row 284  x 0..1     row 288  x 0..4
#   row 285  x 0..3     row 289  x 0..2
#   row 286  x 0..4     row 290  x 0..0
#   row 287  x 0..5
# Mirrored about row 287 that gives a point at x=5 and a slope of about
# 1.5 px per row, so a half height of 3.3 rows. On the right mirrored
# from x=719.
#
# It becomes length 6 and half height 4: the height on the edge has to
# be EVEN, because there is a straight vertical side there and an odd
# band flickers at 25 Hz.
#
# It does NOT touch the large triangle: that has its point at x=9, so
# grey is left between x 6 and 8. In the reference it is the same.
#
# At the top and the bottom there is NO such thing; there is only the
# large triangle there.
TRI_SMALL = (6, 4)                 # length inwards, half height


# INTERLACE. Even picture rows go to field 1 and odd ones to field 2
# (see video.cpp, y = activeY * 2 + field). A horizontal band of an ODD
# number of rows therefore comes out one row thicker in the one field
# than in the other, and you see that as flicker at 25 Hz. So every thin
# horizontal band here is an even number of rows high. For thick areas
# it does not matter: there a row of difference is invisible.
def even_band(ymid, height):
    """Bounds of a band of `height` rows (even) around ymid.

    The top is rounded to the NEAREST even row and not downwards. That
    matters: rounding down put the horizontal lines of Test Card W one
    to two rows too high systematically, because that pitch is 64.35 and
    so rarely falls on a whole number.
    """
    assert height % 2 == 0, "a thin horizontal band must be an even number of rows high"
    y0 = int(round((ymid - height / 2.0) / 2.0)) * 2
    return y0, y0 + height - 1


def fill(b, x0, x1, y0, y1, colour):
    """Fill a rectangle, bounds included."""
    x0 = max(0, int(round(x0))); x1 = min(W - 1, int(round(x1)))
    y0 = max(0, int(round(y0))); y1 = min(H - 1, int(round(y1)))
    if x1 >= x0 and y1 >= y0:
        b[y0:y1 + 1, x0:x1 + 1] = colour


def bordered_edges(white, thickness):
    """Derive the two black borders from the white band.

    Calculating them separately went wrong: the black band and the white
    band were each rounded on their own, and then at x=155 it became
    four pixels on the left and three on the right, and at x=564.5 the
    other way round. Derived like this they are equally wide and
    symmetrical by definition.
    """
    w0, w1 = white
    return (w0 - thickness, w0 - 1), (w1 + 1, w1 + thickness)


def vband(centre, width):
    """Bounds of a vertical band of `width` px around centre.

    Rounded down and not rounded to even: the lines sit on half pixels
    (96.5 and onwards) and with round() the one line jumped and the
    other did not, so that they ended up a pixel apart.
    """
    x0 = int(np.floor(centre - width / 2.0 + 0.5))
    return x0, x0 + width - 1


# --- antialiasing -------------------------------------------------------
#
# ONLY the slanted and the round edges are smoothed. The grid, the
# frame, the colour bars, the greyscale staircase and the castellations
# stay rock hard: those are straight and belong sharp, and softening
# them there only costs palette and runs.
#
# WHY SELECTIVE. The tables are run length encoded over pixel PAIRS (see
# convert_rom_pattern.py). An intermediate colour on an edge breaks the
# run open and adds an extra palette entry, and that is the real limit
# here, not the drawing itself.
#
# For the straight elements supersampling gains nothing anyway: the
# grid, the frame, the colour bars, the side bars, the castellations and
# the greyscale staircase all go through fill(), and that puts them on
# WHOLE pixels. Their edges therefore already fall exactly on a pixel
# boundary, and a finer subpixel raster gives the same result there by
# definition.
#
# WHY THIS IS FREE IN LUMA. In 4:2:2 two pixels share their Cb and Cr,
# but every pixel has its own Y. Almost everything that is smoothed here
# is grey, white or black, neutral colours with Cb = Cr = 128, so the
# intermediate values sit purely in the luma and are exact per pixel.
# Only the top and the bottom triangle stand in coloured areas (magenta
# and green/black respectively); there the chroma runs over a pixel
# pair, and that is exactly the resolution 4:2:2 has.
AA_SUB = 16         # subpixels per axis
# The coverage is then rounded to AA_LEVELS steps. That is the knob that
# keeps palette and runs in hand: fewer steps means neighbouring edge
# pixels get the same intermediate value and so fall in the same palette
# entry.
AA_LEVELS = 64
# How many px the photo crop is stretched outside the inner edge of the
# ring. The edge of the PHOTO does not fall in the card tables but in
# the photo data, and that is unprocessed, so an intermediate colour
# costs nothing there. But there does have to be room to fade out in:
# without a margin the crop stops exactly on the ellipse and there is no
# pixel to put the gradient in. Three px fits well inside the 9 px thick
# ring.
AA_PHOTO_EDGE = 3.0
# HEIGHT OF THE FILTER WINDOW, in picture ROWS.
#
# THERE ARE NEVER 576 ROWS ON THE SCREEN AT ONCE. A field shows only the
# even or only the odd rows, so within a field the next displayed line
# is TWO rows further on. A slanted stripe with dx/dy = 0.9256 therefore
# shifts 1.85 px per displayed line instead of 0.93, and antialiasing
# that is calculated per row says nothing about that. Horizontally the
# chain still helps (the luma bandwidth of a television smears an edge
# of one pixel out by itself), vertically nothing smears: the line
# structure is fixed.
#
# So the filtering is done vertically over more than one row. The window
# is one pixel WIDE and AA_HIGH rows HIGH; a vertical window only
# touches what changes vertically, so upright edges stay exactly as
# sharp as they were and only horizontal and slanted edges get softer.
# That is exactly the direction in which it goes wrong.
#
# THE SIZE COMES FROM THE SOURCE. Measured perpendicular to the stripes,
# the edge in Tcfm.jpg runs from 10 to 90 percent over 2.06 px. With
# AA_HIGH = 2.7 it comes out at 2.03 px, so on the source.
#
# Applies to the hatching and to the circle edge, round and slanted,
# with no hard horizontal edge in them at all. NOT to the triangles: the
# large top and bottom triangle have almost upright flanks, there is
# nothing to gain there, and their base is a horizontal edge that has to
# stay hard. Nor to the small triangles on the side edge: those sit on
# an agreed even band height (see even_band) and you do not want to
# smear that out.
AA_HIGH = 2.7


def ellipse(rx, ry):
    """An ellipse around the centre of the photo circle, as a mask function."""
    return lambda xx, yy: (((xx - CIR_CX) / rx) ** 2 +
                           ((yy - CIR_CY) / ry) ** 2 <= 1.0)


def coverage(bbox, fn, height=1.0):
    """Which part of each pixel in bbox is covered by fn.

    fn is given the CENTRES of AA_SUB x AA_SUB subpixels as two arrays
    that broadcast against each other (x as row, y as column) and
    returns a boolean mask.

    `height` is the height of the filter window in picture ROWS. At 1
    only what lies inside the pixel itself counts: the ordinary box
    filter. At more than 1 every row also looks over its neighbours;
    that is needed because a field only shows every other row (see
    AA_HIGH). The window in x always stays one pixel wide.

    At height > 1 the window runs OUTSIDE bbox, so fn must then contain
    the clip of the box itself, otherwise coverage leaks in from outside
    the box. See draw_corner_blocks().

    The loop over rows keeps the intermediate result small: at
    AA_SUB = 16 the subpixel raster of the photo circle would otherwise
    be 24 million points.
    """
    x0, x1, y0, y1 = bbox
    o = (np.arange(AA_SUB) + 0.5) / AA_SUB - 0.5
    xs = (np.arange(x0, x1 + 1)[:, None] + o[None, :]).ravel()
    out = np.empty((y1 - y0 + 1, x1 - x0 + 1), np.float64)
    step = max(1, 4000000 // (len(xs) * AA_SUB))
    for ya in range(y0, y1 + 1, step):
        yb = min(ya + step - 1, y1)
        ys = (np.arange(ya, yb + 1)[:, None] + (o * height)[None, :]).ravel()
        m = np.asarray(fn(xs[None, :], ys[:, None]), np.float64)
        out[ya - y0:yb - y0 + 1] = m.reshape(yb - ya + 1, AA_SUB,
                                             x1 - x0 + 1, AA_SUB).mean(axis=(1, 3))
    return np.round(out * AA_LEVELS) / AA_LEVELS


def bilinear(m, xx, yy):
    """Read a 720x576 plane at subpixel positions.

    Needed to read the letter F on the subpixel raster: it is not drawn
    but taken from the reference, and then there is no formula to
    supersample, only the source itself.
    """
    x = np.clip(xx, 0, m.shape[1] - 1)
    y = np.clip(yy, 0, m.shape[0] - 1)
    xa = np.floor(x).astype(int)
    ya = np.floor(y).astype(int)
    xb = np.minimum(xa + 1, m.shape[1] - 1)
    yb = np.minimum(ya + 1, m.shape[0] - 1)
    fx, fy = x - xa, y - ya
    return ((m[ya, xa] * (1 - fx) + m[ya, xb] * fx) * (1 - fy) +
            (m[yb, xa] * (1 - fx) + m[yb, xb] * fx) * fy)


def blend(b, bbox, alpha, colour):
    """Lay colour with coverage alpha over what is already there.

    Blending separate edges may be done sequentially here: the black
    stripes in the corner blocks stop 10.3 px before the edge of the
    white band, and the ring of the circle is 9 px thick. So two blended
    edges nowhere touch each other, and then blending separately is the
    same as supersampling in one go.
    """
    x0, x1, y0, y1 = bbox
    a = alpha[:, :, None]
    box = b[y0:y1 + 1, x0:x1 + 1]
    b[y0:y1 + 1, x0:x1 + 1] = box * (1.0 - a) + np.asarray(colour, np.float64) * a


def grat_box():
    """The grey grid box the gratings belong in."""
    left = vband(RX0 + GRAT_CELL * RDX, R_WHITE_V)[1] + 1
    right = vband(RX0 + (GRAT_CELL + 1) * RDX, R_BORDERED_V[0])[0] - 1
    return left, right


def draw_grid(b):
    # ORDER. In the reference the bordered lines win over the plain ones,
    # and the bordered verticals win over everything: the plain line at
    # x=213.5 is broken where the bordered horizontals at 159 and 415
    # cross it, while the bordered line at x=155 runs unbroken from top
    # to bottom. So first all plain lines, then the bordered
    # horizontals, then the bordered verticals.
    vx0 = vband(RX0 + MIDBOX_X[0] * RDX, R_BORDERED_V[1])[1] + 1
    vx1 = vband(RX0 + MIDBOX_X[1] * RDX, R_BORDERED_V[1])[0] - 1
    vy0 = even_band(RY0 + MIDBOX_Y[0] * RDY, R_BORDERED[1])[1] + 1
    vy1 = even_band(RY0 + MIDBOX_Y[1] * RDY, R_BORDERED[1])[0] - 1
    stop = even_band(RY0 + R_STOP_Y * RDY, R_WHITE)[1]

    for n in range(RNY):
        if n in R_BLACK_Y:
            continue
        w0, w1 = even_band(RY0 + n * RDY, R_WHITE)
        fill(b, FRAME_L, FRAME_R, w0, w1, WHITE)
    for n in range(RNX):
        if n in R_BLACK_X:
            continue
        w0, w1 = vband(RX0 + n * RDX, R_WHITE_V)
        fill(b, w0, w1, FRAME_T, FRAME_B if n in R_PLAIN_X_THROUGH else stop, WHITE)

    for n in R_BLACK_Y:
        w0, w1 = even_band(RY0 + n * RDY, R_BORDERED[1])
        top, bottom = bordered_edges((w0, w1), R_BORDERED[0])
        fill(b, FRAME_L, FRAME_R, top[0], top[1], BLACK)
        fill(b, FRAME_L, FRAME_R, bottom[0], bottom[1], BLACK)
        fill(b, FRAME_L, FRAME_R, w0, w1, WHITE)
        if n == MIDBOX_Y[0]:
            fill(b, vx0, vx1, w0, bottom[1], WHITE)
        elif n == MIDBOX_Y[1]:
            fill(b, vx0, vx1, top[0], w1, WHITE)


def draw_bordered_verticals(b):
    """Separate, because they have to go OVER the needle block: on row 72
    the black borders of the line at x=330.5 simply sit in the white
    box."""
    vy0 = even_band(RY0 + MIDBOX_Y[0] * RDY, R_BORDERED[1])[1] + 1
    vy1 = even_band(RY0 + MIDBOX_Y[1] * RDY, R_BORDERED[1])[0] - 1
    stop = even_band(RY0 + R_STOP_Y * RDY, R_WHITE)[1]
    for n in R_BLACK_X:
        bottom_y = FRAME_B if n in R_BLACK_X_THROUGH else stop
        w0, w1 = vband(RX0 + n * RDX, R_BORDERED_V[1])
        left, right = bordered_edges((w0, w1), R_BORDERED_V[0])
        fill(b, left[0], left[1], FRAME_T, bottom_y, BLACK)
        fill(b, right[0], right[1], FRAME_T, bottom_y, BLACK)
        fill(b, w0, w1, FRAME_T, bottom_y, WHITE)
        if n == MIDBOX_X[0]:
            fill(b, w0, right[1], vy0, vy1, WHITE)
        elif n == MIDBOX_X[1]:
            fill(b, left[0], w1, vy0, vy1, WHITE)


def stair_box():
    """The grid box the greyscale staircase fits in, exactly up to the
    white edges. The staircase should fill that box completely."""
    x0 = vband(RX0 + MIDBOX_X[0] * RDX, R_BORDERED_V[1])[1] + R_BORDERED_V[0] + 1
    x1 = vband(RX0 + 2 * RDX, R_WHITE_V)[0] - 1
    y0 = even_band(RY0 + MIDBOX_Y[0] * RDY, R_BORDERED[1])[1] + R_BORDERED[0] + 1
    y1 = even_band(RY0 + MIDBOX_Y[1] * RDY, R_BORDERED[1])[0] - R_BORDERED[0] - 1
    return x0, x1, y0, y1


def draw_corner_blocks(b):
    """The four corner hatchings.

    Two slanted axes: u runs across the band and w runs lengthwise. The
    band width and the stripe pitch sit in u, the slanted cut at the far
    end sits in w. The eight stripes are laid out one by one and not
    with a modulo, so that there really are eight of them and the outer
    two cannot half fall away.

    EVERYTHING here is slanted, so everything is supersampled: the two
    side edges of the band, the slanted cut and the sixteen stripe
    edges. The BOX stays a hard rectangle, because it is a clip along
    the grid and that belongs straight, and that comes out right by
    itself because only what is inside bbox is calculated.
    """
    for (x0, x1, y0, y1), sign, cband, cstripe, wcut, side in CORNER:
        bbox = (x0, x1, y0, y1)

        def uv(xx, yy, sign=sign, wcut=wcut, side=side):
            u = xx - sign * CORNER_S * yy
            d = side * (wcut - (xx + sign * CORNER_S * yy))   # 0 on the cut
            return u, d

        # The BOX is deliberately NOT in the mask but only in bbox. That
        # matters: with AA_HIGH > 1 a hard clip in the mask would leave
        # the edge row of the box only partly filled, and then there is
        # a light stripe along the top of every corner block. Now the
        # band is simply calculated through as if it ran on, and bbox
        # then cuts hard through it: filter and then a window, instead
        # of filtering a window along with it.
        def band_fn(xx, yy, cband=cband):
            u, d = uv(xx, yy)
            return (np.abs(u - cband) <= CORNER_HALF) & (d >= 0.0)

        def stripe_fn(xx, yy, cband=cband, cstripe=cstripe):
            u, d = uv(xx, yy)
            s = np.zeros(u.shape, bool)
            for i in range(CORNER_N):
                mid = cstripe + (i - (CORNER_N - 1) / 2.0) * CORNER_PITCH
                s |= np.abs(u - mid) <= CORNER_WIDTH / 2.0
            return s & (np.abs(u - cband) <= CORNER_HALF) & (d >= CORNER_EDGEW)

        blend(b, bbox, coverage(bbox, band_fn, AA_HIGH), WHITE)
        blend(b, bbox, coverage(bbox, stripe_fn, AA_HIGH), BLACK)


def draw_greyscale(b):
    x0, x1, y0, y1 = stair_box()
    step = (y1 - y0 + 1) / float(len(STAIR_N))
    for i, n in enumerate(STAIR_N):
        fill(b, x0, x1, y0 + i * step, y0 + (i + 1) * step - 1, (n, n, n))
    for (x0, x1, y0, y1, n) in (STAIR_PATCH_L, STAIR_PATCH_D):
        fill(b, x0, x1, y0, y1, (n, n, n))


def draw_gratings(b):
    GRAT_X0, GRAT_X1 = grat_box()
    n = np.arange(GRAT_X0, GRAT_X1 + 1) - GRAT_X0
    for i, mhz in enumerate(GRAT_MHZ):
        y0, y1 = GRAT_BANDS[i], GRAT_BANDS[i + 1] - 1
        v = np.clip(126.0 + GRAT_AMP * np.sin(2 * np.pi * mhz * 1e6 * n / FS), 0, 255)
        b[y0:y1 + 1, GRAT_X0:GRAT_X1 + 1] = v[None, :, None]


def stamp_letter_f(b, ref):
    """Take the letter F from the reference unchanged.

    It is reduced to the three colours that occur here, white, grey and
    black, so that the area stays flat and encodes along with the rest.

    That reduction is done on the SUBpixel raster: the source is read
    bilinearly at AA_SUB x AA_SUB positions per pixel, every subpixel
    goes to the nearest of the three levels, and the average of those is
    the pixel. Reducing per whole pixel turned a serif letter with round
    transitions into a chopped staircase. This is the same supersampling
    as for the corner blocks, only here the shape does not sit in a
    formula but in the source.

    This way the area really stays flat, because the JPEG noise in the
    grey around the letter is measured at 104 to 132 on a level of 126
    and that all falls within the same level, so every subpixel comes
    out at GREY, while the edges of the letter get a soft gradient.
    """
    x0, x1, y0, y1 = F_STAMP
    lum = ref @ np.array([0.299, 0.587, 0.114])
    levels = np.array([WHITE[0], GREY[0], BLACK[0]])
    bbox = (x0, x1, y0, y1)
    g = np.zeros((y1 - y0 + 1, x1 - x0 + 1))
    for i, n in enumerate(levels):
        def fn(xx, yy, i=i):
            v = bilinear(lum, xx, yy)
            return np.argmin(np.abs(v[..., None] - levels), axis=-1) == i
        g += coverage(bbox, fn) * n
    b[y0 - F_UP:y1 + 1 - F_UP, x0:x1 + 1] = g[:, :, None]


def draw_triangles(b):
    """The overscan triangles, with smooth slanted flanks.

    The SLANTED flanks are supersampled; the BASE stays hard. That last
    part is not obvious: the base of the top and the bottom triangle is
    a horizontal edge, and an edge that smears out over half a row would
    make an extra thin band there. Hence the extra half pixel in the
    base condition: it puts the clip on the pixel BOUNDARY instead of on
    the pixel CENTRE, so that the base row takes part fully and the edge
    stays exactly where it was.

    The slanted flanks themselves run through the base row, so that row
    is soft at the ends. That is intended.
    """
    cx, cy = 360, 287
    L, Hf = float(TRI_LONG), float(TRI_HALF)
    top, bot, lft, rgt = TRI_BASE
    # distance to the base, from 0 on the base to L in the point
    for bbox, fn in (
            ((int(cx - Hf) - 1, int(cx + Hf) + 1, top - int(L), top),
             lambda xx, yy: (yy <= top + 0.5) & (yy >= top - L) &
                            (np.abs(xx - cx) <= Hf * (yy - (top - L)) / L)),
            ((int(cx - Hf) - 1, int(cx + Hf) + 1, bot, bot + int(L)),
             lambda xx, yy: (yy >= bot - 0.5) & (yy <= bot + L) &
                            (np.abs(xx - cx) <= Hf * ((bot + L) - yy) / L)),
            ((lft - int(L), lft, int(cy - Hf) - 1, int(cy + Hf) + 1),
             lambda xx, yy: (xx <= lft + 0.5) & (xx >= lft - L) &
                            (np.abs(yy - cy) <= Hf * (xx - (lft - L)) / L)),
            ((rgt, rgt + int(L), int(cy - Hf) - 1, int(cy + Hf) + 1),
             lambda xx, yy: (xx >= rgt - 0.5) & (xx <= rgt + L) &
                            (np.abs(yy - cy) <= Hf * ((rgt + L) - xx) / L))):
        blend(b, bbox, coverage(bbox, fn), WHITE)

    # The small triangles on the picture edge, point inwards.
    #
    # The height on the edge is set to an EVEN number of rows through
    # even_band. That is not an idle worry here: on the edge there is a
    # straight vertical side, and as an odd band that would flicker at
    # 25 Hz, just like any other thin horizontal band.
    #
    # The x in the slope formula is CLAMPED on the picture edge. Without
    # that, the half pixel that column 0 covers to the left of x=0 would
    # make the triangle slightly higher there, and then the row just
    # above and below the straight side gets a trace of coverage:
    # exactly the thin horizontal edge that starts flickering at 25 Hz.
    # With the clamp the coverage there is exactly zero and the side
    # stays eight whole rows high.
    sl, sh = TRI_SMALL
    sy0, sy1 = even_band(cy, 2 * sh)
    scy = (sy0 + sy1) / 2.0
    for bbox, fn in (
            ((0, sl, sy0 - 1, sy1 + 1),
             lambda xx, yy: np.abs(yy - scy) <=
                            sh * (1.0 - np.clip(xx, 0.0, sl) / float(sl))),
            ((W - 1 - sl, W - 1, sy0 - 1, sy1 + 1),
             lambda xx, yy: np.abs(yy - scy) <=
                            sh * (1.0 - np.clip(W - 1 - xx, 0.0, sl) / float(sl)))):
        blend(b, bbox, coverage(bbox, fn), WHITE)


def draw_card(ref):
    b = np.zeros((H, W, 3), np.float64)
    b[:] = GREY

    draw_grid(b)
    draw_corner_blocks(b)
    draw_greyscale(b)
    draw_gratings(b)

    fill(b, NEEDLE[0], NEEDLE[1], NEEDLE[2], NEEDLE[3], WHITE)
    # Order: the white box, then the bordered verticals over it (in the
    # reference their black borders simply sit in the white box), and
    # only then the black bar, which on row 100 is unbroken black from
    # x 297 to 422, so it wins over the white of the lines.
    draw_bordered_verticals(b)
    fill(b, NEEDLE_BAR[0], NEEDLE_BAR[1], NEEDLE_BAR[2], NEEDLE_BAR[3], BLACK)

    # The black bordered grid line on y=287 runs THROUGH the bars:
    # measured in column 620 the black is broken from 285.5 to 288.6.
    # So the bars are laid down in two pieces, with the white of the
    # grid line in between.
    w0, w1 = even_band(RY0 + 3 * RDY, R_BORDERED[1])
    for (bx0, bx1) in CROSS_BARS:
        fill(b, bx0, bx1, CROSS_Y[0], w0 - 1, BLACK)
        fill(b, bx0, bx1, w1 + 1, CROSS_Y[1], BLACK)

    # NO cut out around the F: the black borders of the grid lines at
    # x=330.5 and x=389 belong there, from the outer edge of the circle
    # up to and including the bottom of the white bar below it, so the
    # letter is stamped over them as they are.
    stamp_letter_f(b, ref)

    # What is set to grey here is EXACTLY the area the photograph will
    # cover, not an ellipse of its own. Otherwise an edge of one pixel
    # is left between the photograph and the ring: too narrow to stand
    # still, so it was visibly shimmering.
    #
    # The OUTER edge of the ring is round and is supersampled. The INNER
    # edge is not: it falls under the photograph, so the white of the
    # ring runs on there in the photo data past the ellipse and goes
    # over into the picture there (see blend_photo_edge). That is why
    # photo_mask() is AA_PHOTO_EDGE px wider than the inside of the ring.
    bbox = (int(CIR_CX - CIR_RX) - 1, int(CIR_CX + CIR_RX) + 1,
            int(CIR_CY - CIR_RY) - 1, int(CIR_CY + CIR_RY) + 1)
    blend(b, bbox, coverage(bbox, ellipse(CIR_RX, CIR_RY), AA_HIGH), WHITE)
    b[photo_mask()] = GREY

    # frame
    fill(b, FRAME_L, FRAME_R, FRAME_T, FRAME_T + FRAME_D - 1, WHITE)
    fill(b, FRAME_L, FRAME_R, FRAME_B - FRAME_D + 1, FRAME_B, WHITE)
    fill(b, FRAME_L, FRAME_L + FRAME_D - 1, FRAME_T, FRAME_B, WHITE)
    fill(b, FRAME_R - FRAME_D + 1, FRAME_R, FRAME_T, FRAME_B, WHITE)

    # top colour bar
    for i in range(len(BAR_C)):
        fill(b, BAR_X[i], BAR_X[i + 1] - 1, 0, BAR_H - 1, BAR_C[i])

    # side bars
    for (y0, y1, c) in SIDE_LEFT:
        fill(b, 0, SIDE_L - 1, y0, y1, c)
    for (y0, y1, c) in SIDE_RIGHT:
        fill(b, SIDE_R, W - 1, y0, y1, c)

    # bottom castellation, with white in the corners as in the reference
    fill(b, CAST_MID[0], CAST_MID[1] - 1, CAST_Y0, CAST_Y1, BLACK)
    for edge, direction in ((CAST_MID[0], -1), (CAST_MID[1], 1)):
        n = 0
        while True:
            x0 = edge + direction * n * CAST_W
            x1 = x0 + direction * CAST_W
            lo, hi = min(x0, x1), max(x0, x1)
            if hi < 0 or lo > W:
                break
            fill(b, lo, hi - 1, CAST_Y0, CAST_Y1, GREEN if n % 2 == 0 else BLACK)
            n += 1
    for (x0, x1) in CAST_END:
        fill(b, x0, x1, CAST_Y0, CAST_Y1, BLACK)
    for (x0, x1) in CAST_WHITE:
        fill(b, x0, x1, CAST_WHITE_Y0, CAST_Y1, WHITE)

    draw_triangles(b)
    return b


def to_422(rgb):
    R, G, B = rgb[..., 0] / 255.0, rgb[..., 1] / 255.0, rgb[..., 2] / 255.0
    Y = 0.299 * R + 0.587 * G + 0.114 * B
    CB = 128 + 112 * (B - Y) / 0.886
    CR = 128 + 112 * (R - Y) / 0.701
    Y = 16 + 219 * Y
    # Halve the chroma by averaging pairs, not by throwing one away.
    CB = (CB[:, 0::2] + CB[:, 1::2]) / 2
    CR = (CR[:, 0::2] + CR[:, 1::2]) / 2
    L = np.zeros((H, W * 2), np.uint8)
    L[:, 1::2] = np.clip(np.round(Y), 16, 235)
    L[:, 0::4] = np.clip(np.round(CB), 16, 240)
    L[:, 2::4] = np.clip(np.round(CR), 16, 240)
    return L


def photo_mask():
    """Which pixels the photograph will cover, rounding included.

    write_photo() stretches every row to an even start and an even
    length, because a 4:2:2 pixel pair may not be cut in half. That
    stretched area is what really gets overwritten, so that is what the
    ring has to close up against.

    The crop is AA_PHOTO_EDGE px wider than the inner edge of the ring.
    That extra edge is made white in blend_photo_edge() and carries the
    gradient from the photograph to the white of the ring.
    """
    yy, xx = np.mgrid[0:H, 0:W]
    inside = ellipse(CIR_RX - CIR_RING + AA_PHOTO_EDGE,
                     CIR_RY - CIR_RING + AA_PHOTO_EDGE)(xx, yy)
    span = np.zeros((H, W), bool)
    for y in range(H):
        idx = np.flatnonzero(inside[y])
        if not len(idx):
            continue
        xa, xb = int(idx.min()), int(idx.max())
        xa -= xa & 1
        xb += 1 - (xb & 1)
        span[y, xa:xb + 1] = True
    return span


def place_photo(path):
    """Put the separate photo scan in its place in the 720x576 raster."""
    src = Image.open(path).convert("RGB")
    sx, sy = PHOTO_SCALE
    w, h = int(round(src.width * sx)), int(round(src.height * sy))
    im = np.asarray(src.resize((w, h), Image.LANCZOS)).astype(np.float64)
    canvas = np.zeros((H, W, 3), np.float64)
    x0, y0 = int(round(PHOTO_CORNER[0])), int(round(PHOTO_CORNER[1]))
    x1, y1 = min(W, x0 + w), min(H, y0 + h)
    canvas[y0:y1, x0:x1] = im[:y1 - y0, :x1 - x0]
    print("  photo: %dx%d scaled to %dx%d, corner (%d, %d)"
          % (src.width, src.height, w, h, x0, y0))
    return canvas


def blend_photo_edge(canvas):
    """Let the edge of the photograph fade into the white of the ring.

    The inner edge of the ring is round; clipping it hard per picture
    row on an EVEN pixel gives steps two pixels wide, well visible at
    the top and the bottom of the circle where the edge runs
    almost horizontally. That cannot be cured with a drawing command
    here: those pixels do not come from the card tables but from the
    photo data, because the photograph is laid over it as separate
    spans.

    So the gradient is put in the PHOTOGRAPH. Outside the ellipse it
    becomes the white of the ring, inside it the photograph, and on the
    edge a mixture by coverage. That is free as well: the photo data
    does not go through the palette and not through the run length
    encoding, so intermediate colours cost no run at all there.
    """
    bbox = (int(CIR_CX - CIR_RX), int(CIR_CX + CIR_RX),
            int(CIR_CY - CIR_RY), int(CIR_CY + CIR_RY))
    a = coverage(bbox, ellipse(CIR_RX - CIR_RING, CIR_RY - CIR_RING),
                AA_HIGH)[:, :, None]
    x0, x1, y0, y1 = bbox
    out = np.empty((H, W, 3), np.float64)
    out[:] = WHITE
    out[y0:y1 + 1, x0:x1 + 1] = (canvas[y0:y1 + 1, x0:x1 + 1] * a +
                                 np.asarray(WHITE, np.float64) * (1.0 - a))
    return out


def write_photo(ref, inside, name="testcardf", card="Test Card F"):
    """Cut out the photo circle and write it as spans per picture row.

    Both cards share this writer, so the card the header names is passed
    in; hardcoding it here put "Test Card F" in the Test Card W header.
    """
    photo_lines = to_422(ref)
    rows = [y for y in range(H) if inside[y].any()]
    y0, y1 = rows[0], rows[-1]

    chunks, x0s, lens = [], [], []
    for y in range(y0, y1 + 1):
        idx = np.flatnonzero(inside[y])
        xa, xb = int(idx.min()), int(idx.max())
        xa -= xa & 1                      # start on an even pixel,
        xb += 1 - (xb & 1)                # otherwise the chroma phase is wrong
        x0s.append(xa)
        lens.append(xb - xa + 1)
        chunks.append(photo_lines[y, xa * 2:(xb + 1) * 2])
    data = np.concatenate(chunks)
    off = np.concatenate([[0], np.cumsum([len(b) for b in chunks])[:-1]])

    write_table("%s/%s_photo.inc.h" % (OUT_DIR, name), data.tobytes())
    print("  photo: row %d..%d, %d bytes (%.0f kB)" % (y0, y1, data.size, data.size / 1024))

    nl = chr(10)
    with io.open("%s/%s_photo.S" % (OUT_DIR, name), "w", encoding="utf-8", newline=nl) as fh:
        fh.write(".section .rodata" + nl + ".balign 4" + nl +
                 ".global %s_photo" % name + nl + "%s_photo:" % name + nl +
                 '#include "%s_photo.inc.h"' % name + nl +
                 "// %s_photo.inc.h: %d bytes of table%s" % (name, data.size, nl))

    with io.open("%s/%s_photo.h" % (OUT_DIR, name), "w", encoding="utf-8", newline=nl) as fh:
        w = fh.write
        w("// GENERATED by tools/make_testcardf.py -- do not edit by hand." + nl)
        w("//" + nl)
        w("// The photograph in the middle of %s. One span per picture" % card + nl)
        w("// row: the first pixel, the length in pixels, and where the data sits." + nl)
        w("// The first pixel and the length are both even, because a 4:2:2 pixel" + nl)
        w("// pair may not be cut in half." + nl)
        w("#pragma once" + nl + nl)
        w("#include <stdint.h>" + nl + nl)
        P = name.upper()
        w("static const int %s_PHOTO_Y0 = %d;" % (P, y0) + nl)
        w("static const int %s_PHOTO_ROWS = %d;" % (P, y1 - y0 + 1) + nl + nl)
        w('extern "C" { extern const uint8_t %s_photo[]; }' % name + nl + nl)
        for vname, arr, typ in (("x0", x0s, "uint16_t"), ("len", lens, "uint16_t"),
                                ("off", off, "uint32_t")):
            w("static const %s %s_photo_%s[%s_PHOTO_ROWS] = {" % (typ, name, vname, P) + nl)
            for i in range(0, len(arr), 16):
                w("    " + ", ".join(str(int(v)) for v in arr[i:i + 16]) + "," + nl)
            w("};" + nl + nl)
    print("  written: src/testcards/%s_photo.inc.h, .S and .h" % name)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("reference", help="Test Card F as 720x576; only the photograph is used")
    p.add_argument("photo", nargs="?", help="separate, sharper scan of the centre photograph")
    p.add_argument("--png", help="also write the result as a PNG to compare")
    a = p.parse_args()

    im = Image.open(a.reference).convert("RGB")
    if im.size != (W, H):
        raise SystemExit("reference must be %dx%d, not %dx%d" % (W, H, im.width, im.height))
    ref = np.asarray(im).astype(np.float64)

    card = draw_card(ref)
    print("card drawn from %d different colours"
          % len(np.unique(card.reshape(-1, 3), axis=0)))
    print("  colour bars at 95 percent saturation")
    print("  gratings at %s MHz, all six at the same amplitude"
          % ", ".join("%g" % m for m in GRAT_MHZ))

    inside = photo_mask()
    canvas = blend_photo_edge(place_photo(a.photo) if a.photo else ref)

    if a.png:
        show = card.copy()
        show[inside] = canvas[inside]
        Image.fromarray(np.clip(show, 0, 255).astype(np.uint8)).save(a.png)
        print("  %s written" % a.png)

    cfg = {"title": "BBC Test Card F",
           "note": "card drawn; only the photograph comes from a reference"}
    encode_and_write("testcardf", cfg, [to_422(card)], 0)
    write_photo(canvas, inside)


if __name__ == "__main__":
    sys.exit(main())

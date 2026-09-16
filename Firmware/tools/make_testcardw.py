"""
Draws BBC Test Card W, the 16:9 version of Test Card F.


    python tools/make_testcardw.py reference.png photo.jpg --png compare.png

Produces:
  src/testcards/testcardw_{pal,idx,len,start,line}.bin + _data.h + _data.S  (the card)
  src/testcards/testcardw_photo.inc.h + _photo.S + _photo.h                   (the photo)

THE SAME SET-UP AS TEST CARD F: everything is drawn except the photo and
the letter, and the shared building blocks come from make_testcardf.py,
including the supersampling of the sloping and round edges.

THE PHOTO IS NOT SHARED WITH F, and it cannot be. The circle of W
measures 110.9 x 157.8 px in our raster, that of F 149 x 156.5. That
difference is no coincidence but the anamorphic ratio: at 16:9 a picture
pixel is 1.42 times as wide as it is high and at 4:3 only 1.07 times, so
a circle that is round on the screen is much narrower in pixels on W.
What IS shared is the SOURCE: the same scan of 304x304 and the same
fitting procedure (correlation 0.9945).

THE MEASURED VALUES:

  colour bars 100 percent saturation. Published (Test Card W went from
              95 to 100 percent relative to F). The source itself does
              not quite reach that, yellow measures (255, 241, 0)
              instead of (255, 255, 0) and magenta (255, 39, 255), but
              that is the video coding of the recording and not a card
              value, so the published 100 percent stands here.

  gratings    1.5 / 2.5 / 3.5 / 4 / 4.5 / 5.25 MHz, the published series
              for Test Card F AND ITS VARIANTS.
              NOTE: the HD reference itself does not reach those
              frequencies. It is an HD redrawing in which the gratings
              were set as a PIXEL PATTERN rather than in MHz, and it
              carries square waves rather than sines. Hence the
              published series is used and not the measured one.

  grey stairs six steps 213, 171, 128, 85, 42 and 0, measured in the
              flat core of column 290..350 of the reference (min and max
              per row are equal there). No published specification, so
              these come from the source. There are NO small calibration
              patches in the staircase, unlike on Test Card F: over the
              whole staircase min equals max.

GEOMETRY, measured in videoframe_2227.png (1280x720, square pixels,
perfectly flat areas). Converting to our raster is x times 0.5625 and
y times 0.8. Behind every measurement stands where it comes from.

NOT YET THE SAME AS THE SOURCE
  BBC ONE     The reference is a broadcast picture and carries the
              channel logo "BBC ONE" under the W (three white blocks
              with B, B, C plus the text ONE, measured in the source at
              x 467..675 and y 610..672 HD). That is a STATION
              IDENTIFICATION and not part of the test card, and it is
              the only place where this source shows something that
              another W broadcast shows differently. It is NOT drawn
              here. Consequence: in the bottom row of grid boxes we have
              grey where the source has the blocks. This is deliberate;
              should the owner want the logo after all, it has to come
              out of the source as a stamp, like the letter W.
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
from make_testcardf import (W, H, WHITE, BLACK, even_band, vband, to_422,
                            write_photo, saturate, bordered_edges,
                            coverage, blend, AA_HIGH, AA_PHOTO_EDGE)

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src",
                       "testcards")

GREY = (128.0, 128.0, 128.0)           # field, measured in dozens of places

# 100 percent saturation, against 95 on Test Card F.
YELLOW = saturate((255, 255, 0), 1.0)
CYAN = saturate((0, 255, 255), 1.0)
GREEN = saturate((0, 255, 0), 1.0)
MAGENTA = saturate((255, 0, 255), 1.0)
RED = saturate((255, 0, 0), 1.0)
BLUE = saturate((0, 0, 255), 1.0)

# --- grid ---------------------------------------------------------------
# Vertically: the fifteen visible lines measured as the centroid of the
# white profile (38.56 118.51 198.80 279.00 358.77 438.82 519.00 599.02
# 758.98 839.18 919.23 998.99 1079.17 1159.49 1239.36 in the source) with
# a straight line put through them: x = 38.622 + 80.0495 n, largest
# residual 0.23 px. Times 0.5625 gives the figures below. The frame
# coincides with the outermost two lines, just as on Test Card F.
RX0, RDX, RNX = 21.725, 45.028, 16
# Horizontally: nine lines at 37.62 118.28 198.54 279.00 359.40 439.81
# 520.24 600.42 680.95, fit y = 37.766 + 80.3992 n, largest residual 0.15.
RY0, RDY, RNY = 30.212, 64.319, 9

# Widths, measured as half-amplitude crossings (128 between 0 and 255) on
# cross profiles averaged over dozens of rows or columns:
#   bare horizontal line    7.37 / 7.16 / 7.35 HD  -> ours 5.9   -> 6
#   bare vertical line      7.23 / 7.27 HD         -> ours 4.08  -> 4
R_WHITE = 6
R_WHITE_V = 4
# The black-bordered lines measure in the source black 3.56 / white 3.78
# / black 3.56 HD (three horizontal lines, spread 0.06). Vertically that
# is 3.42 / 3.83 / 3.52.
#   horizontal times 0.8    -> black 2.85  white 3.02  black 2.85
#   vertical times 0.5625   -> black 1.92  white 2.15  black 1.98
# Vertically that simply becomes 2, 2, 2. Horizontally both the white and
# each black border MUST be an even number of rows (see even_band), so
# the choice is 2/4/2 or 4/2/4. 2/4/2 counts 8 rows against the measured
# 8.66 and 4/2/4 counts 10; 2/4/2 wins on the total width and on the sum
# of the individual deviations. On Test Card F that choice came out the
# other way round.
R_BORDERED = (2, 4)                    # (thickness of ONE black border, white height)
R_BORDERED_V = (2, 2)

# WHICH lines have black borders. Measured in cross profiles outside the
# centre box:
#   horizontal   n = 2, 4 and 6. The MIDDLE one (n=4, the line at half
#                height) does have them: on column 200..270 I measure
#                black 354..357, white 358..361, black 362..365.
#   vertical     n = 3, 7, 8 and 12. Those at 7 and 8 are simply
#                black-bordered grid lines that run from the frame down
#                to grid line n=7: on row 45 up to and including row 600
#                of the source I measure at x=599 consistently black
#                594..596, white 598..600, black 602..604.
R_BLACK_Y = (2, 4, 6)
R_BLACK_X = (3, 7, 8, 12)

# The centre box, bounded by four black-bordered lines. On the INSIDE of
# those there is no black border but white: on column 860..905 (inside)
# n=2 has black only above and n=6 only below, while both have black on
# two sides on column 200..270 (outside).
MIDBOX_X = (3, 12)
MIDBOX_Y = (2, 6)

# Which vertical lines run through to the frame. In the bottom row of
# boxes (source y 600..680, so below grid line n=7) only n = 0, 1, 2, 3,
# 12, 13, 14 and 15 are present; 4 up to and including 11 are gone there.
# The white BBC blocks sit exactly on x 467..675 and so fall across n=6,
# 7 and 8; outside those blocks (row 601..608) there is nothing to see.
R_THROUGH_X = (0, 1, 2, 3, 12, 13, 14, 15)
R_STOP_Y = 7                           # the rest stops at this line

# The frame IS the outermost grid line, drawn over its full length.
# Measured: left 34.74..42.17 (ours 19.54..23.72, centre 21.63), right
# 1235.87..1243.25 (ours 695.18..699.33, centre 697.25), top
# 33.93..41.30 (ours 27.14..33.04, centre 30.09) and bottom
# 677.41..684.74 (ours 541.93..547.79, centre 544.86). The grid lines
# themselves lie at 21.73 / 697.14 / 30.21 / 544.76, so that is the same
# line.
FRAME_DV, FRAME_DH = 4, 6              # thickness upright (4.18), lying (5.89)
FRAME_L, FRAME_R = vband(RX0, FRAME_DV)[0], vband(RX0 + (RNX - 1) * RDX, FRAME_DV)[1]
FRAME_T, FRAME_B = even_band(RY0, FRAME_DH)[0], even_band(RY0 + (RNY - 1) * RDY, FRAME_DH)[1]

# --- colour bar at the top ----------------------------------------------
# The eight boundaries averaged over rows 5..25: 64.5 228.5 392.5 556.5
# 721 885.5 1049.5 1214. Times 0.5625 gives the series below.
# The bar runs down to where the frame begins: the transition to white
# lies at 33.93 (ours 27.14) and the top bar of the frame starts at
# row 28.
BAR_H = FRAME_T                        # the bar runs to where the frame begins
BAR_X = (0, 36, 128, 221, 313, 406, 498, 590, 683, 720)
BAR_C = (WHITE, YELLOW, CYAN, GREEN, MAGENTA, RED, BLUE, BLACK, WHITE)

# --- side bars ----------------------------------------------------------
# Column 10 and column 1270 of the reference. The side bar touches the
# frame: on the left it runs to 34.6 (ours 19.5) and on the right it
# starts at 1243.3 (ours 699.3), right up against the frame bars.
SIDE_L, SIDE_R = FRAME_L, FRAME_R + 1
# Boundaries from the same columns, each time the middle between two flat
# stretches, times 0.8.
SIDE_LEFT = ((28, 38, BLACK), (39, 80, RED), (81, 125, BLACK), (126, 237, RED),
             (238, 331, BLACK), (332, 442, BLUE), (443, 488, BLACK),
             (489, 529, BLUE), (530, 553, BLACK))
SIDE_RIGHT = ((28, 56, BLACK), (57, 92, YELLOW), (93, 179, BLACK), (180, 237, WHITE),
              (238, 337, BLACK), (338, 395, WHITE), (396, 483, BLACK),
              (484, 518, YELLOW), (519, 553, BLACK))

# AND THERE IS A NARROWER STRIP ALONG THE OUTERMOST PICTURE EDGE, with a
# different pattern from the side bar within it.
# Measured on column 2 and column 1277 against column 10 and column 1270:
#   left    x 0..5.68 (ours 0..3.2 wide). Where the wide bar is black
#           between the two red blocks (source y 100.3..158.3) there is
#           WHITE here, and where it is black between the two blue blocks
#           (553.5..611.5) there is MAGENTA.
#   right   x 1272.11..1280 (ours 715.6..720, so 4.4 wide). White over
#           115.3..297.5 and magenta over 495.5..603.5.
# For the rest the narrow strip is exactly the same as the wide one.
EDGE_L = (0, 2)
EDGE_R = (716, 719)
EDGE_LEFT = ((80, 126, WHITE), (443, 489, MAGENTA))
EDGE_RIGHT = ((92, 237, WHITE), (396, 482, MAGENTA))

# --- bottom strip -------------------------------------------------------
# TWO halves above each other. At the top a grey gradient from black on
# the left to white on the right, below it the same gradient reversed;
# the division lies at row 702.8 of the source, that is 562.
#
# It is a GRADIENT and not a series of blocks: on row 694 the value runs
# linearly from 12 at x=100 to 250 at x=600, which works back to 0 at
# x=74.8 and 255 at x=610.5 (ours 42.1 and 343.4).
STRIP_Y0, STRIP_MID, STRIP_Y1 = 548, 562, 575
# The white blocks in the corners only start at row 693 of the source
# (ours 554.4); above that the side bar simply continues in black. Same
# peculiarity as the castellations on Test Card F.
STRIP_WHITE_Y0 = 554
STRIP_WHITE_L = (0, 11)                # white 0..20 HD
STRIP_BLACK_L = (12, 41)               # black 22..74.8 HD
STRIP_GREY = (42, 343)
# This is the BOTTOM OVERSCAN TRIANGLE in a black box. On row 694 the
# white is 31 px wide and on row 712 only 9; so it is not a small bar but
# a triangle with its tip at the bottom edge.
STRIP_BLACK_M = (344, 375)             # black 610.5..667.5 HD
STRIP_COLOURBOX = (376, 677)           # colour 668..1204.5 HD
# Colour gradient green to magenta, fitted linearly per channel on row
# 694 (R 0.2583 per HD px, G -0.15625, B 0.2396), worked back to the ends
# of the box.
STRIP_COLOUR_FROM = (57.7, 169.3, 62.4)
STRIP_COLOUR_TO = (196.1, 85.5, 190.8)
STRIP_BLACK_R = (678, 706)
STRIP_WHITE_R = (707, 719)

# --- overscan triangles -------------------------------------------------
# They sit in all four edges and are drawn on Test Card F as well. They
# point OUTWARD: the base stands against the card and the tip on the
# picture edge.
#
#   top     in the magenta bar. On row 0 the white is 2 px wide and on
#           row 34 it is 43; tip at x 638.7 (ours 359.2), half base width
#           21.5 HD (ours 12.1), base on the bottom of the colour bar.
#   bottom  in the bottom strip. On row 685 half width 20.5 HD, on row
#           719 only 0.5; tip at x 639.5 (ours 359.7).
#   left    in the side bar. From a tip at x=0 (white 359..359) to
#           339..380 at x=34, so half height 21 HD (ours 16.8) against
#           the frame.
#   right   mirrored from x=1279; half height 22 HD (ours 17.6).
# The upright base of left and right must be an EVEN number of rows, so
# half height 17 (the average of 16.8 and 17.6 is 17.2).
TRIANGLE_X = 359.4                     # centre of the top and bottom triangle
TRIANGLE_Y = RY0 + 4 * RDY             # centre of left and right, 287.5
TRIANGLE_TOP_HALF = 12.9
TRIANGLE_BOTTOM_HALF = 11.9
TRIANGLE_SIDE_HALF = 17

# --- grey staircase -----------------------------------------------------
# Column 290..350; the steps are completely flat there (min = max per row).
STAIR_N = (213, 171, 128, 85, 42, 0)

# --- gratings -----------------------------------------------------------
# In the grid box between line 11 and 12, worked out from the grid itself
# so that it cannot run over it. Vertically they fill exactly the same
# box as the grey staircase (source y 203.5..516.5, ours 162.8..413.2),
# so the six band boundaries come out of stair_box() and are not
# separate.
GRAT_CELL = 11
GRAT_MHZ = (1.5, 2.5, 3.5, 4.0, 4.5, 5.25)
GRAT_AMP = 100.0
FS = 13.5e6

# --- needle block -------------------------------------------------------
#   white box    x 493.3..784.6, y 85.6..150.7 HD  -> ours 277.5..441.4 /
#                68.5..120.6
#   black bar    x 551.7..726.6, y 99.6..137.0     -> ours 310.3..408.7 /
#                79.7..109.6
#   green patch  x 628.5..649.7, y 107.6..128.5    -> ours 353.5..365.5 /
#                86.1..102.8
# The green patch is 21x21 px in the source, so SQUARE on the screen; in
# our anamorphic raster that becomes 11.9 wide and 16.7 high.
NEEDLE_BOX = (277, 441, 68, 119)
NEEDLE_BAR = (310, 408, 80, 109)
NEEDLE_GREEN = (354, 365, 86, 101)

# --- marker bars at half height -----------------------------------------
# On the INSIDE of grid lines n=2 and n=13 stands a short black bar, cut
# through by the white centre line n=4. Measured on the left at
# x 200.6..204.2 (ours 112.8..114.9) and y 318.5..399.5 (ours
# 254.8..319.6), on the right at x 1073.5..1077.5 (ours 603.8..606.2).
# Same element as the "crosses" on Test Card F, but there they are two
# per side and here one.
MARKER_N = (2, 13)
MARKER_Y = (255, 319)
MARKER_W = 2

# --- circle -------------------------------------------------------------
# The reference has square pixels, so the circle really is round there.
# The outer edge measured at the half amplitude between grey and white:
#   column 639 (through the centre)  top 161.9  bottom 557.2 -> cy 359.55
#                                                                R 197.65
#   row 300 / 330 / 380 / 415        half width 187.8 194.6 195.3 189.2
# Those four rows fit R = 197.0 with a residual of 0.7 px, so R 197.2.
# The centre lies on the card axis: the grid fit gives 359.4 and the
# colour bar boundaries 359.6.
CIR_CX, CIR_CY = 359.4, 287.6
CIR_RX, CIR_RY = 110.9, 157.8
# Ring thickness 7.7 HD (column 639 top 7.9 and bottom 7.4; row 330 left
# 7.9 and right 7.2). Radially that is 4.33 in x and 6.16 in y in our
# raster.
CIR_RINGX, CIR_RINGY = 4.33, 6.16

# --- letter W -----------------------------------------------------------
# Not drawn but copied, for the same reason as the F on Test Card F: it
# is a serif letter. The window has been adjusted so that it falls
# BETWEEN the black borders of grid lines n=7 and n=8: those borders lie
# at x 594..604 and 673..683 (ours 334..340 and 378.6..384.4) and are now
# drawn by draw_grid, so the stamp does not have to include them. The
# letter itself sits in the source at x 610..668 and y 555..592, that is
# ours 343..375.8 and 444..473.6.
W_STAMP = (341, 377, 444, 477)

# --- diamonds at half height --------------------------------------------
# The white centre line widens in the two outermost box columns on each
# side into a chain of diamonds, with the tips in the middle of each box
# and widest on the grid lines.
#
# Measured as the height of the white area per column, left of line n=1
# and right of it: the top edge moves away from the line at 0.59 HD per
# HD px (0.571 on one side, 0.607 on the other) and cuts the line at
# y 335.5, while the centre lies at 359.4. Half height on the line is
# therefore 23.9 HD, ours 19.1. Between the diamonds only the line itself
# is left: 4 HD half height, ours 3.2.
# In our raster the slope becomes 0.59 times 0.8 / 0.5625 = 0.84.
#
# Outside those boxes there is nothing in the source: from x=203 the
# white there is just 3 px high again.
DIAMOND_Y = TRIANGLE_Y
DIAMOND_HALF, DIAMOND_TIP, DIAMOND_SLOPE = 19.1, 3.0, 0.84
DIAMOND_BOXES = ((0, 2), (13, 15))     # grid line n0 up to and including n1

# --- corner blocks ------------------------------------------------------
# A diagonal white band with EIGHT black strokes in it, cut off
# perpendicularly at the far end and clipped by the frame at the near
# side. Same model as on Test Card F.
#
# THE SLOPE. The stroke centres per picture row give dx/dy = 1.000 in the
# source in all four corners, so the strokes stand there at exactly 45
# degrees, and that source has square pixels. In our anamorphic raster
# that becomes 0.8 / 0.5625 = 1.4222.
#
# THE CUT. The right length measure is w = y + sign * 1.4222 x, because
# that one is perpendicular to the strokes on the SCREEN.
#
# ALL THE FIGURES, measured per corner separately on the black strokes
# (black with white within 8 px on both sides, so that the black borders
# of the grid lines and the side bars do not count in):
#   number of strokes  8, 8, 8, 8
#   pitch              12.334 HD in all four (spread 0.26)  -> ours 9.889
#   stroke width       6.04 HD at the half amplitude        -> ours 4.83
#                      that is 49 percent duty
#   band width         the white band measures 62.6 HD half width in the
#                      top left corner and 61.3 in the bottom right; on
#                      average 61.9 HD -> ours 49.6.
#   cut                the black stops perpendicular to the strokes at a
#                      fixed w: 282, -990, 437 and 1715 HD, that is
#                      225.6 / -792 / 349.6 / 1372 in our raster, for all
#                      eight strokes of a corner exactly the same value,
#                      which confirms that the cut is square to them. The
#                      WHITE runs another 16.5 HD (ours 12.9) past it,
#                      just as it keeps 12.6 of white at the sides. CORNER
#                      holds the place of the WHITE cut, so the measured
#                      black plus 12.9.
CORNER_S = 0.8 / 0.5625                # 1.4222
CORNER_N = 8
CORNER_PITCH = 9.889
CORNER_W = 4.83
CORNER_HALF = 49.6                     # half width of the WHITE band
CORNER_EDGEW = 12.9                    # this far before the cut the black stops
# Per corner: the box (which bbox clips hard through), the sign of the
# band direction, the centre of the band in v = y - sign*1.4222 x, and
# the place of the cut in w = y + sign*1.4222 x with which side the band
# lies on (+1 = w smaller). The centres come from a fit on the eight
# stroke centres per corner, with a residual of at most 0.20 px.
CORNER_BOX_X = (FRAME_L + FRAME_DV, RX0 + 2 * RDX)
CORNER_BOX_Y = (FRAME_T + FRAME_DH, RY0 + 2 * RDY)
CORNER = (
    ((CORNER_BOX_X[0], CORNER_BOX_X[1], CORNER_BOX_Y[0], CORNER_BOX_Y[1]),
     +1, 0.447, 238.5, +1),                                        # top left
    ((W - 1 - CORNER_BOX_X[1], W - 1 - CORNER_BOX_X[0], CORNER_BOX_Y[0], CORNER_BOX_Y[1]),
     -1, 1022.044, -779.1, +1),                                    # top right
    ((CORNER_BOX_X[0], CORNER_BOX_X[1], H - 1 - CORNER_BOX_Y[1], H - 1 - CORNER_BOX_Y[0]),
     -1, 574.752, 336.7, -1),                                      # bottom left
    ((W - 1 - CORNER_BOX_X[1], W - 1 - CORNER_BOX_X[0], H - 1 - CORNER_BOX_Y[1],
      H - 1 - CORNER_BOX_Y[0]),
     +1, -447.645, 1359.1, -1),                                    # bottom right
)

# --- photo --------------------------------------------------------------
# Fitted by laying the 304x304 scan over a range of scales and shifts and
# searching for the highest correlation with what stands in the circle of
# the source. Best: 0.99455.
PHOTO_SCALE = (0.770, 1.094)
PHOTO_CORNER = (247.0, 127.0)


def fill(b, x0, x1, y0, y1, colour):
    x0 = max(0, int(round(x0))); x1 = min(W - 1, int(round(x1)))
    y0 = max(0, int(round(y0))); y1 = min(H - 1, int(round(y1)))
    if x1 >= x0 and y1 >= y0:
        b[y0:y1 + 1, x0:x1 + 1] = colour


def midbox():
    """The INNER boundaries of the centre box, in pixels."""
    vx0 = vband(RX0 + MIDBOX_X[0] * RDX, R_BORDERED_V[1])[1] + 1
    vx1 = vband(RX0 + MIDBOX_X[1] * RDX, R_BORDERED_V[1])[0] - 1
    vy0 = even_band(RY0 + MIDBOX_Y[0] * RDY, R_BORDERED[1])[1] + 1
    vy1 = even_band(RY0 + MIDBOX_Y[1] * RDY, R_BORDERED[1])[0] - 1
    return vx0, vx1, vy0, vy1


def bordered_vertical(b, n, y0, y1):
    """Draw a black-bordered vertical grid line over y0..y1."""
    x = RX0 + n * RDX
    w0, w1 = vband(x, R_BORDERED_V[1])
    left, right = bordered_edges((w0, w1), R_BORDERED_V[0])
    fill(b, left[0], left[1], y0, y1, BLACK)
    fill(b, right[0], right[1], y0, y1, BLACK)
    fill(b, w0, w1, y0, y1, WHITE)
    return (w0, w1), left, right


def draw_grid(b):
    vx0, vx1, vy0, vy1 = midbox()

    for n in range(RNY):
        y = RY0 + n * RDY
        if n in R_BLACK_Y:
            w0, w1 = even_band(y, R_BORDERED[1])
            top, bottom = bordered_edges((w0, w1), R_BORDERED[0])
            fill(b, FRAME_L, FRAME_R, top[0], top[1], BLACK)
            fill(b, FRAME_L, FRAME_R, bottom[0], bottom[1], BLACK)
            fill(b, FRAME_L, FRAME_R, w0, w1, WHITE)
            if n == MIDBOX_Y[0]:
                fill(b, vx0, vx1, w0, bottom[1], WHITE)
            elif n == MIDBOX_Y[1]:
                fill(b, vx0, vx1, top[0], w1, WHITE)
            else:
                # The centre line is a bare line INSIDE the box: on
                # column 860..905 I measure white 355.7..363.0 there and
                # no black, while outside the box there is 3.5/3.8/3.5.
                fill(b, vx0, vx1, top[0], bottom[1], GREY)
                bare0, bare1 = even_band(y, R_WHITE)
                fill(b, vx0, vx1, bare0, bare1, WHITE)
        else:
            w0, w1 = even_band(y, R_WHITE)
            fill(b, FRAME_L, FRAME_R, w0, w1, WHITE)

    stop = even_band(RY0 + R_STOP_Y * RDY, R_WHITE)[1]
    for n in range(RNX):
        bottom_y = FRAME_B if n in R_THROUGH_X else stop
        if n in R_BLACK_X:
            (w0, w1), left, right = bordered_vertical(b, n, FRAME_T, bottom_y)
            if n == MIDBOX_X[0]:
                fill(b, w0, right[1], vy0, vy1, WHITE)
            elif n == MIDBOX_X[1]:
                fill(b, left[0], w1, vy0, vy1, WHITE)
        else:
            w0, w1 = vband(RX0 + n * RDX, R_WHITE_V)
            fill(b, w0, w1, FRAME_T, bottom_y, WHITE)


def draw_markers(b):
    """The short black marker bars on the inside of n=2 and n=13."""
    w0, w1 = even_band(RY0 + 4 * RDY, R_BORDERED[1])   # the white centre line
    for n in MARKER_N:
        l0, l1 = vband(RX0 + n * RDX, R_WHITE_V)
        if n < RNX / 2:
            x0, x1 = l1, l1 + MARKER_W - 1           # inward = to the right
        else:
            x0, x1 = l0 - MARKER_W + 1, l0           # inward = to the left
        fill(b, x0, x1, MARKER_Y[0], w0 - 1, BLACK)
        fill(b, x0, x1, w1 + 1, MARKER_Y[1], BLACK)


def stair_box():
    """The grid box the grey staircase fits in, exactly up to the white edges.

    The grey staircase AND the gratings both fill this box; in the source
    they run from y 203.5 to 516.5, that is 162.8 to 413.2 in our raster.

    TOP and BOTTOM do not have the same formula here, and that is
    deliberate. Inside the centre box the white of a bordered line grows
    only 1.8 HD (1.4 rows) past its outermost white edge in the source,
    while draw_grid makes the whole inner black border white, which is a
    row or two too generous. At the top that happens to come out well
    through the rounding of even_band (white 156..161, staircase from
    162, measured 162.8); at the bottom it does not (white 412..417,
    while the source lets the staircase run to 413.2 and only lets the
    white start at 413.3). That is why the staircase ends against the
    WHITE band here and not against the black border: that gives
    162..413 against the measured 162.8..413.2, where the symmetrical
    formula is 2.2 rows out at the bottom.
    """
    x0 = vband(RX0 + MIDBOX_X[0] * RDX, R_BORDERED_V[1])[1] + R_BORDERED_V[0] + 1
    x1 = vband(RX0 + (MIDBOX_X[0] + 1) * RDX, R_WHITE_V)[0] - 1
    y0 = even_band(RY0 + MIDBOX_Y[0] * RDY, R_BORDERED[1])[1] + R_BORDERED[0] + 1
    y1 = even_band(RY0 + MIDBOX_Y[1] * RDY, R_BORDERED[1])[0] - 1
    return x0, x1, y0, y1


def grat_box():
    left = vband(RX0 + GRAT_CELL * RDX, R_WHITE_V)[1] + 1
    right = vband(RX0 + (GRAT_CELL + 1) * RDX, R_BORDERED_V[0])[0] - 1
    return left, right


def draw_greystair(b):
    x0, x1, y0, y1 = stair_box()
    step = (y1 - y0 + 1) / float(len(STAIR_N))
    for i, n in enumerate(STAIR_N):
        fill(b, x0, x1, y0 + i * step, y0 + (i + 1) * step - 1, (n, n, n))


def draw_gratings(b):
    x0, x1 = grat_box()
    _, _, y0, y1 = stair_box()
    band = (y1 - y0 + 1) / float(len(GRAT_MHZ))
    n = np.arange(x0, x1 + 1) - x0
    for i, mhz in enumerate(GRAT_MHZ):
        ya = int(round(y0 + i * band))
        yb = int(round(y0 + (i + 1) * band)) - 1
        # Around the FIELD grey 128 and not around 126: the latter came
        # from Test Card F, where the field really is 126.
        v = np.clip(GREY[0] + GRAT_AMP * np.sin(2 * np.pi * mhz * 1e6 * n / FS), 0, 255)
        b[ya:yb + 1, x0:x1 + 1] = v[None, :, None]


def draw_diamonds(b):
    """The chain of diamonds at half height, in the two outermost box columns.

    The flanks are sloping and are therefore supersampled, just like the
    hatching and the circle. The CLAMP at DIAMOND_TIP is deliberately NOT
    in the mask: that would give a LYING edge and that has to stay hard.
    Where the diamond is narrower than the grid line it falls inside the
    white that draw_grid has already put down hard there.
    """
    for (n0, n1) in DIAMOND_BOXES:
        x0 = max(FRAME_L, int(round(RX0 + n0 * RDX)))
        x1 = min(FRAME_R, int(round(RX0 + n1 * RDX)))
        bbox = (x0, x1, int(DIAMOND_Y - DIAMOND_HALF) - 2, int(DIAMOND_Y + DIAMOND_HALF) + 2)

        def fn(xx, yy):
            d = np.abs((xx - RX0) % RDX)
            d = np.minimum(d, RDX - d)
            return np.abs(yy - DIAMOND_Y) <= DIAMOND_HALF - DIAMOND_SLOPE * d

        blend(b, bbox, coverage(bbox, fn, AA_HIGH), WHITE)


def ellipse(rx, ry):
    return lambda xx, yy: (((xx - CIR_CX) / rx) ** 2 +
                           ((yy - CIR_CY) / ry) ** 2 <= 1.0)


def triangle(b, tip, base, half, direction):
    """An overscan triangle: tip on the picture edge, base against the card.

    `direction` is "h" if the triangle points horizontally (left/right)
    and "v" if it points vertically (top/bottom). It is NOT supersampled:
    the flanks run almost parallel to the axis they follow, and the base
    is a hard edge that has to stay sharp, the same trade-off as on Test
    Card F.
    """
    yy, xx = np.mgrid[0:H, 0:W]
    if direction == "h":
        t = np.abs(xx - tip[0]) / float(abs(base - tip[0]))
        m = (np.abs(xx - tip[0]) <= abs(base - tip[0])) & \
            (np.abs(yy - tip[1]) <= half * t)
    else:
        t = np.abs(yy - tip[1]) / float(abs(base - tip[1]))
        m = (np.abs(yy - tip[1]) <= abs(base - tip[1])) & \
            (np.abs(xx - tip[0]) <= half * t)
    b[m] = WHITE


def draw_cornerblocks(b):
    """The four corner hatchings, supersampled just as on Test Card F.

    v runs ACROSS the band (the band width and the stroke pitch are in
    it) and w runs along its LENGTH (the cut is in that one). Both axes
    use the same slope 1.4222 but with the other sign, so that they stand
    perpendicular to each other on the screen.
    """
    for (x0, x1, y0, y1), sign, cband, wcut, side in CORNER:
        bbox = (int(round(x0)), int(round(x1)), int(round(y0)), int(round(y1)))

        def vw(xx, yy, sign=sign, wcut=wcut, side=side):
            v = yy - sign * CORNER_S * xx
            d = side * (wcut - (yy + sign * CORNER_S * xx))    # 0 on the cut
            return v, d

        def band_fn(xx, yy, cband=cband):
            v, d = vw(xx, yy)
            return (np.abs(v - cband) <= CORNER_HALF) & (d >= 0.0)

        def stroke_fn(xx, yy, cband=cband):
            v, d = vw(xx, yy)
            s = np.zeros(v.shape, bool)
            for i in range(CORNER_N):
                mid = cband + (i - (CORNER_N - 1) / 2.0) * CORNER_PITCH
                s |= np.abs(v - mid) <= CORNER_W / 2.0
            return s & (np.abs(v - cband) <= CORNER_HALF) & (d >= CORNER_EDGEW)

        blend(b, bbox, coverage(bbox, band_fn, AA_HIGH), WHITE)
        blend(b, bbox, coverage(bbox, stroke_fn, AA_HIGH), BLACK)


def stamp(b, ref, box, choice=None):
    x0, x1, y0, y1 = box
    cut = ref[y0:y1 + 1, x0:x1 + 1]
    k = np.array(choice if choice is not None else [WHITE, GREY, BLACK])
    d = np.linalg.norm(cut[:, :, None, :] - k[None, None, :, :], axis=3)
    b[y0:y1 + 1, x0:x1 + 1] = k[np.argmin(d, 2)]


def draw_card(ref):
    b = np.zeros((H, W, 3), np.float64)
    b[:] = GREY

    # The hatching lies ON TOP of the grid: in the reference the black
    # strokes cut straight through the white grid lines.
    draw_grid(b)
    draw_diamonds(b)
    draw_cornerblocks(b)
    draw_greystair(b)
    draw_gratings(b)
    draw_markers(b)

    # Needle block. First the white box, then the black-bordered lines
    # n=7 and n=8 over it again, in the source their borders are simply
    # visible inside the white box (row 95..99 and 137..150), and only
    # then the black bar, which wins over that.
    fill(b, NEEDLE_BOX[0], NEEDLE_BOX[1], NEEDLE_BOX[2], NEEDLE_BOX[3], WHITE)
    for n in (7, 8):
        bordered_vertical(b, n, NEEDLE_BOX[2], NEEDLE_BOX[3])
    fill(b, NEEDLE_BAR[0], NEEDLE_BAR[1], NEEDLE_BAR[2], NEEDLE_BAR[3], BLACK)
    fill(b, NEEDLE_GREEN[0], NEEDLE_GREEN[1], NEEDLE_GREEN[2], NEEDLE_GREEN[3], GREEN)

    stamp(b, ref, W_STAMP)

    # circle ring; the inside is exactly what the photo will cover later
    bbox = (int(CIR_CX - CIR_RX) - 1, int(CIR_CX + CIR_RX) + 1,
            int(CIR_CY - CIR_RY) - 1, int(CIR_CY + CIR_RY) + 1)
    blend(b, bbox, coverage(bbox, ellipse(CIR_RX, CIR_RY), AA_HIGH), WHITE)
    b[photo_mask()] = GREY

    # frame
    fill(b, FRAME_L, FRAME_R, FRAME_T, FRAME_T + FRAME_DH - 1, WHITE)
    fill(b, FRAME_L, FRAME_R, FRAME_B - FRAME_DH + 1, FRAME_B, WHITE)
    fill(b, FRAME_L, FRAME_L + FRAME_DV - 1, FRAME_T, FRAME_B, WHITE)
    fill(b, FRAME_R - FRAME_DV + 1, FRAME_R, FRAME_T, FRAME_B, WHITE)

    for i in range(len(BAR_C)):
        fill(b, BAR_X[i], BAR_X[i + 1] - 1, 0, BAR_H - 1, BAR_C[i])
    triangle(b, (TRIANGLE_X, -1.0), float(BAR_H), TRIANGLE_TOP_HALF, "v")

    for (y0, y1, k) in SIDE_LEFT:
        fill(b, 0, SIDE_L - 1, y0, y1, k)
    for (y0, y1, k) in SIDE_RIGHT:
        fill(b, SIDE_R, W - 1, y0, y1, k)
    for (y0, y1, k) in EDGE_LEFT:
        fill(b, EDGE_L[0], EDGE_L[1], y0, y1, k)
    for (y0, y1, k) in EDGE_RIGHT:
        fill(b, EDGE_R[0], EDGE_R[1], y0, y1, k)
    triangle(b, (-0.5, TRIANGLE_Y), float(SIDE_L), TRIANGLE_SIDE_HALF, "h")
    triangle(b, (W - 0.5, TRIANGLE_Y), float(SIDE_R), TRIANGLE_SIDE_HALF, "h")

    for (y0, y1, flipped) in ((STRIP_Y0, STRIP_MID - 1, False),
                                (STRIP_MID, STRIP_Y1, True)):
        fill(b, STRIP_WHITE_L[0], STRIP_WHITE_L[1], y0, y1, BLACK)
        fill(b, STRIP_BLACK_L[0], STRIP_BLACK_L[1], y0, y1, BLACK)
        gx0, gx1 = STRIP_GREY
        t = (np.arange(gx0, gx1 + 1) - gx0) / float(gx1 - gx0)
        v = 255.0 * ((1.0 - t) if flipped else t)
        b[y0:y1 + 1, gx0:gx1 + 1] = v[None, :, None]
        fill(b, STRIP_BLACK_M[0], STRIP_BLACK_M[1], y0, y1, BLACK)
        kx0, kx1 = STRIP_COLOURBOX
        t = (np.arange(kx0, kx1 + 1) - kx0) / float(kx1 - kx0)
        kl = np.stack([STRIP_COLOUR_FROM[i] + t * (STRIP_COLOUR_TO[i] - STRIP_COLOUR_FROM[i])
                       for i in range(3)], 1)
        b[y0:y1 + 1, kx0:kx1 + 1] = kl[None, :, :]
        fill(b, STRIP_BLACK_R[0], STRIP_BLACK_R[1], y0, y1, BLACK)
        fill(b, STRIP_WHITE_R[0], STRIP_WHITE_R[1], y0, y1, BLACK)
    fill(b, STRIP_WHITE_L[0], STRIP_WHITE_L[1], STRIP_WHITE_Y0, STRIP_Y1, WHITE)
    fill(b, STRIP_WHITE_R[0], STRIP_WHITE_R[1], STRIP_WHITE_Y0, STRIP_Y1, WHITE)
    triangle(b, (TRIANGLE_X, float(H)), float(STRIP_Y0), TRIANGLE_BOTTOM_HALF, "v")
    return b


def photo_mask():
    """Which pixels the photo covers, including the stretch out to even."""
    yy, xx = np.mgrid[0:H, 0:W]
    inside = ellipse(CIR_RX - CIR_RINGX + AA_PHOTO_EDGE,
                     CIR_RY - CIR_RINGY + AA_PHOTO_EDGE)(xx, yy)
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


def blend_photo_edge(canvas):
    """Let the edge of the photo run out into the white of the ring.

    Same reason as on Test Card F: the inner edge of the ring is round
    but is clipped hard to an EVEN pixel per picture row, and that gave
    two pixel steps at the top and bottom of the circle. Those pixels do
    not come from the card tables but from the photo data, so the
    gradient has to be in the PHOTO, and there an intermediate colour
    costs nothing, because that data does not go through the palette and
    not through the run length encoding.
    """
    bbox = (int(CIR_CX - CIR_RX), int(CIR_CX + CIR_RX),
            int(CIR_CY - CIR_RY), int(CIR_CY + CIR_RY))
    a = coverage(bbox, ellipse(CIR_RX - CIR_RINGX, CIR_RY - CIR_RINGY),
                AA_HIGH)[:, :, None]
    x0, x1, y0, y1 = bbox
    out = np.empty((H, W, 3), np.float64)
    out[:] = WHITE
    out[y0:y1 + 1, x0:x1 + 1] = (canvas[y0:y1 + 1, x0:x1 + 1] * a +
                                 np.asarray(WHITE, np.float64) * (1.0 - a))
    return out


def place_photo(path, scale=PHOTO_SCALE, corner=PHOTO_CORNER):
    src = Image.open(path).convert("RGB")
    w, h = int(round(src.width * scale[0])), int(round(src.height * scale[1]))
    im = np.asarray(src.resize((w, h), Image.LANCZOS)).astype(np.float64)
    canvas = np.zeros((H, W, 3), np.float64)
    x0, y0 = int(round(corner[0])), int(round(corner[1]))
    x1, y1 = min(W, x0 + w), min(H, y0 + h)
    canvas[y0:y1, x0:x1] = im[:y1 - y0, :x1 - x0]
    print("  photo: %dx%d scaled to %dx%d, corner (%d, %d)"
          % (src.width, src.height, w, h, x0, y0))
    return canvas


def main():
    p = argparse.ArgumentParser()
    p.add_argument("reference", help="Test Card W, is scaled to 720x576")
    p.add_argument("photo", help="separate scan of the centre photo")
    p.add_argument("--png", help="also write the result as a PNG")
    a = p.parse_args()

    im = Image.open(a.reference).convert("RGB")
    if im.size != (W, H):
        print("reference %dx%d -> %dx%d scaled" % (im.width, im.height, W, H))
        im = im.resize((W, H), Image.LANCZOS)
    ref = np.asarray(im).astype(np.float64)

    card = draw_card(ref)
    print("card drawn out of %d colours; bars at 100 percent saturation"
          % len(np.unique(card.reshape(-1, 3), axis=0)))
    print("gratings at %s MHz, all six at the same amplitude"
          % ", ".join("%g" % m for m in GRAT_MHZ))

    inside = photo_mask()
    photo_image = blend_photo_edge(place_photo(a.photo))

    if a.png:
        show = card.copy()
        show[inside] = photo_image[inside]
        Image.fromarray(np.clip(show, 0, 255).astype(np.uint8)).save(a.png)
        print("  %s written" % a.png)

    cfg = {"title": "BBC Test Card W",
           "note": "16:9; card drawn, photograph and letter from a reference"}
    encode_and_write("testcardw", cfg, [to_422(card)], 0)
    write_photo(photo_image, inside, "testcardw", "Test Card W")


if __name__ == "__main__":
    sys.exit(main())

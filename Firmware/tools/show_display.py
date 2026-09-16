"""
Draws what the status display will show, on the PC.

    python tools/show_display.py pm5644g00
    python tools/show_display.py barsred --out C:/tmp

  display.png       the 284x76 panel one pixel for one pixel
  display_big.png   the same at four times the size, to read it by

WHY THIS EXISTS. The panel is 284 by 76 with a proportional font on it,
so whether a line of text fits is not something to work out on paper. It
is also the one part of the display that can be checked without any
hardware: this reproduces src/display.cpp exactly, the same font table,
the same widths, the same line height and the same way of sampling the
miniature out of a finished picture row. What comes out here is what the
panel gets.

It does NOT prove the panel itself. The offsets, the colour order and
which way up it sits can only be settled on the bench, with key L.
"""

import argparse
import io
import os
import re
import sys

import numpy as np
from PIL import Image

from show_testcard import decode

HERE = os.path.dirname(os.path.abspath(__file__))
FONT28 = os.path.join(HERE, "..", "src", "display_font28.h")
FONT16 = os.path.join(HERE, "..", "src", "display_font16.h")
FONT12 = os.path.join(HERE, "..", "src", "display_font12.h")
BG = os.path.join(HERE, "..", "src", "display_bg.h")

# The layout, the same numbers as display.h and display.cpp.
W, H = 284, 76
THUMB_W, THUMB_H = 101, 76
TEXT_X = THUMB_W + 3
RULE_X = THUMB_W + 1
TEXT_W = W - TEXT_X
# Three bands: two of text and a row of badges along the bottom. The
# same numbers as display.cpp.
TITLE_H = 34
INFO_H = 26
BADGE_H = 16
BAND_H = [TITLE_H, INFO_H, BADGE_H]
BAND_TOP = [0, TITLE_H, TITLE_H + INFO_H]

BADGE_PAD = 1

COL_TEXT = (255, 255, 255)
COL_TITLE = (190, 160, 0)
COL_DIM = (150, 150, 150)
COL_BADGE_ON = (255, 40, 40)
COL_BADGE_OFF = (80, 80, 80)
COL_ID_OFF = COL_BADGE_OFF
COL_RULE = (0, 96, 255)
COL_TITLE_RULE = (0, 200, 60)
COL_BACK = (0, 0, 0)

W_BG, H_BG = TEXT_W, H
BACKGROUND = []


def read_font(path, prefix):
    """The glyph table and the metrics out of a generated header."""
    s = io.open(path, encoding="utf-8").read()
    glyphs = {}
    body = s.split(prefix + "_GLYPHS[] = {")[1].split("};")[0]
    for m in re.finditer(r"\{'(\\?.)', *(-?\d+), *(-?\d+), *(-?\d+), *(-?\d+), *(-?\d+), *(\d+)\}",
                         body):
        c = m.group(1)[-1]
        glyphs[c] = tuple(int(m.group(i)) for i in range(2, 8))
    pixels = bytes(int(v, 16) for v in
                   re.findall(r"0x([0-9A-Fa-f]{2}),",
                              s.split(prefix + "_PIXELS[] = {")[1].split("};")[0]))
    m = re.search(r"SmoothFont " + prefix + r" = \{\s*" + prefix + r"_GLYPHS, *"
                  + prefix + r"_PIXELS,([^}]*)\}", s)
    nums = [int(v) for v in re.findall(r"-?\d+", m.group(1))]
    count, ascent, descent, height, space, ink_up, ink_down = nums[:7]
    return {"glyphs": glyphs, "pixels": pixels, "ascent": ascent,
            "height": height, "space": space,
            "ink_up": ink_up, "ink_down": ink_down,
            "digit": max(glyphs[d][2] for d in "0123456789")}


def read_bg():
    """The background of the text area, as it is stored: panel byte order."""
    t = io.open(BG, encoding="ascii").read()
    body = t.split("DISPLAY_BG[DISPLAY_BG_W * DISPLAY_BG_H] = {")[1]
    v = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{4}),", body)]
    if len(v) != W_BG * H_BG:
        raise SystemExit("background is %d pixels, expected %d" % (len(v), W_BG * H_BG))
    return v


def advance(c, font, fixed_digits):
    if fixed_digits and c.isdigit():
        return font["digit"]
    g = font["glyphs"].get(c)
    return g[2] if g else font["space"]


# The panel is fed RGB565 with the high byte first, and blend565() in
# display.cpp mixes in that order. Reproducing both here means a shade
# that is wrong on the bench is wrong in this picture too.
def rgb565(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((v >> 8) | (v << 8)) & 0xFFFF


def swap565(v):
    return ((v >> 8) | (v << 8)) & 0xFFFF


def blend565(back, fore, alpha):
    if alpha == 0:
        return back
    if alpha == 255:
        return fore
    bk, fo = swap565(back), swap565(fore)
    out = 0
    for shift, mask in ((11, 0x1F), (5, 0x3F), (0, 0x1F)):
        b = (bk >> shift) & mask
        f = (fo >> shift) & mask
        out |= (b + ((f - b) * alpha + 127) // 255) << shift
    return swap565(out)


def to_rgb(v):
    p = swap565(v)
    return (((p >> 11) & 0x1F) * 255 // 31, ((p >> 5) & 0x3F) * 255 // 63,
            (p & 0x1F) * 255 // 31)


def badge_width(text, font):
    return sum(advance(c, font, False) for c in text) + 2 * BADGE_PAD + 2


def badge_baseline(badges, font):
    """The same as badgeBaseline() in display.cpp: over the labels in use,
    not over the metrics of the face, because a label has no descenders."""
    up = down = 0
    for text, _ in badges:
        for c in text:
            g = font["glyphs"].get(c)
            if not g:
                continue
            up = max(up, g[4])
            down = max(down, g[1] - g[4])
    if up == 0:
        up = font["ascent"]
    return up + (BADGE_H - (up + down)) // 2


def draw_badges(img, badges, font):
    """The same as drawBadges() in display.cpp."""
    top = BAND_TOP[2]
    strip = [list(BACKGROUND[(top + y) * TEXT_W:(top + y + 1) * TEXT_W])
             for y in range(BADGE_H)]

    shown = [(t, on) for t, on in badges if t]
    if shown:
        # Justified, the same as drawBadges() in display.cpp: first against
        # the left edge, last against the right, the rest shared out.
        widths = [badge_width(t, font) for t, _ in shown]
        total = sum(widths)
        slack = max(0, TEXT_W - total)
        n = len(shown)
        before = 0
        for placed, (text, on) in enumerate(shown):
            w = widths[placed]
            x = before + (slack * placed) // (n - 1) if n > 1 else (TEXT_W - w) // 2
            before += w
            colour = rgb565(*(COL_BADGE_ON if on else COL_BADGE_OFF))
            for c in range(w):
                if x + c < TEXT_W:
                    strip[0][x + c] = colour
                    strip[BADGE_H - 1][x + c] = colour
            for r in range(BADGE_H):
                if x < TEXT_W:
                    strip[r][x] = colour
                if x + w - 1 < TEXT_W:
                    strip[r][x + w - 1] = colour

            baseline = badge_baseline(shown, font)
            pen = x + 1 + BADGE_PAD
            for c in text:
                g = font["glyphs"].get(c)
                if g:
                    gw, gh, gadv, dx, dy, off = g
                    for r in range(gh):
                        yy = baseline - dy + r
                        if not 0 <= yy < BADGE_H:
                            continue
                        for col in range(gw):
                            a = font["pixels"][off + r * gw + col]
                            if not a:
                                continue
                            xx = pen + dx + col
                            if not 0 <= xx < TEXT_W:
                                continue
                            strip[yy][xx] = colour if a == 255 else blend565(strip[yy][xx], colour, a)
                    pen += gadv
                else:
                    pen += font["space"]

    for y in range(BADGE_H):
        for x2 in range(TEXT_W):
            img[top + y, TEXT_X + x2] = to_rgb(strip[y][x2])


def draw_info(img, left, right, dim, font):
    """The info band in two runs, the same as drawInfo() in display.cpp:
    the card number in grey and the identification behind it, dark grey
    when the card on screen does not draw that text."""
    top = BAND_TOP[1]
    strip = [list(BACKGROUND[(top + y) * TEXT_W:(top + y + 1) * TEXT_W])
             for y in range(INFO_H)]
    band = font["ink_up"] + font["ink_down"]
    slack = INFO_H - band
    baseline = font["ink_up"] + (slack // 2 if slack >= 0 else -((-slack + 1) // 2))

    def run(text, colour, pen):
        fore = rgb565(*colour)
        for c in text:
            adv = advance(c, font, True)
            if pen + adv > TEXT_W:
                break
            g = font["glyphs"].get(c)
            if g:
                w, h, gadv, dx, dy, off = g
                lead = (adv - gadv) // 2 if c.isdigit() else 0
                for r in range(h):
                    yy = baseline - dy + r
                    if not 0 <= yy < INFO_H:
                        continue
                    for col in range(w):
                        a = font["pixels"][off + r * w + col]
                        if not a:
                            continue
                        xx = pen + lead + dx + col
                        if not 0 <= xx < TEXT_W:
                            continue
                        strip[yy][xx] = fore if a == 255 else blend565(strip[yy][xx], fore, a)
            pen += adv
        return pen

    pen = run(left, COL_DIM, 0)
    if right:
        pen += advance(" ", font, False) * 2
        run(right, COL_ID_OFF if dim else COL_DIM, pen)

    for y in range(INFO_H):
        for x in range(TEXT_W):
            img[top + y, TEXT_X + x] = to_rgb(strip[y][x])
    return pen


def draw_text(img, line, text, colour, font, fixed_digits):
    """The same as drawSmooth() in display.cpp, clipping included."""
    box_h = BAND_H[line]
    top = BAND_TOP[line]
    # The same rule as drawSmooth() in display.cpp: centre the ink band,
    # and where the box falls short round the halving towards the bottom.
    band = font["ink_up"] + font["ink_down"]
    slack = box_h - band
    baseline = font["ink_up"] + (slack // 2 if slack >= 0 else -((-slack + 1) // 2))
    fore = rgb565(*colour)
    # The strip starts as a copy of the background, the same memcpy that
    # drawSmooth() does.
    strip = [list(BACKGROUND[(top + y) * TEXT_W:(top + y + 1) * TEXT_W])
             for y in range(box_h)]

    pen = 0
    for c in text:
        adv = advance(c, font, fixed_digits)
        if pen + adv > TEXT_W:
            break
        g = font["glyphs"].get(c)
        if g:
            w, h, gadv, dx, dy, off = g
            lead = (adv - gadv) // 2 if (fixed_digits and c.isdigit()) else 0
            for r in range(h):
                yy = baseline - dy + r
                if not 0 <= yy < box_h:
                    continue
                for col in range(w):
                    a = font["pixels"][off + r * w + col]
                    if not a:
                        continue
                    xx = pen + lead + dx + col
                    if not 0 <= xx < TEXT_W:
                        continue
                    strip[yy][xx] = fore if a == 255 else blend565(strip[yy][xx], fore, a)
        pen += adv

    # The line under the title, on the last row of that band; the same as
    # startRegion() in display.cpp.
    if line == 0:
        strip[box_h - 1] = [rgb565(*COL_TITLE_RULE)] * TEXT_W

    for y in range(box_h):
        for x in range(TEXT_W):
            img[top + y, TEXT_X + x] = to_rgb(strip[y][x])
    return pen


def thumbnail(name, variant):
    """The miniature, built the way displaySampleRow() builds it.

    Field one only, so the even picture rows. Every even row of a band
    is added in, and across the width every second sample of the span a
    column covers, so about fourteen of the fifty-four picture samples
    behind one point of the miniature. Averaging and not picking is what
    keeps a grid line one pixel wide from being all or nothing.

    """
    frame = decode(name, variant)
    thumb_x = [(i * 720) // THUMB_W for i in range(THUMB_W)] + [720]
    out = np.zeros((THUMB_H, THUMB_W, 3), np.uint8)
    for band in range(THUMB_H):
        rows = [y for y in range(576) if (y * THUMB_H) // 576 == band and not (y & 1)]
        for i in range(THUMB_W):
            sb = sr = n = 0
            row_y = []
            for y in rows:
                row = frame[y]
                taps = []
                for x in range(thumb_x[i], thumb_x[i + 1], 2):
                    pair = (x >> 1) * 4
                    taps.append(int(row[pair + (3 if (x & 1) else 1)]))
                    sb += int(row[pair])
                    sr += int(row[pair + 2])
                    n += 1
                # Kept on the four sample scale, no division per
                # column, the same as display.cpp does it.
                row_y.append(sum(taps))
            if not n:
                continue
            Cb, Cr = sb // n - 128, sr // n - 128
            # The division by four happens here and only here, as in
            # flushBand().
            Y = sum(row_y) // (len(row_y) * 4)
            y298 = 298 * (Y - 16)
            out[band, i] = (
                np.clip((y298 + 409 * Cr + 128) >> 8, 0, 255),
                np.clip((y298 - 100 * Cb - 208 * Cr + 128) >> 8, 0, 255),
                np.clip((y298 + 516 * Cb + 128) >> 8, 0, 255),
            )
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("name", help="the card in the miniature, so pm5644g00 and so on")
    p.add_argument("--variant", type=int, default=0)
    p.add_argument("--lines", nargs=2,
                   default=["EBU bars red", "16/33  0001 / PE5PVB"],
                   help="the title and the line under it")
    p.add_argument("--id-off", action="store_true",
                   help="draw the identification in dark grey, as when the card "
                        "on screen does not show it")
    p.add_argument("--badges", default="RGB:1,TXT:1,WSS:0,ITS:0,CLK:1,SHOW:0,USB:1",
                   help="label:1 for lit, label:0 for dark grey, comma separated")
    p.add_argument("--out", default=os.path.expanduser("~/Downloads"))
    a = p.parse_args()

    global BACKGROUND
    BACKGROUND = read_bg()
    title = read_font(FONT28, "DISPFONT28")
    small = read_font(FONT16, "DISPFONT16")
    img = np.zeros((H, W, 3), np.uint8)
    img[:, :] = COL_BACK
    img[0:THUMB_H, 0:THUMB_W] = thumbnail(a.name, a.variant)
    img[:, RULE_X] = COL_RULE
    for y in range(H):
        for x in range(TEXT_W):
            img[y, TEXT_X + x] = to_rgb(BACKGROUND[y * TEXT_W + x])

    print("text area is %d px wide, bands of %s px" % (TEXT_W, BAND_H))
    used = draw_text(img, 0, a.lines[0], COL_TITLE, title, False)
    print("  title  %-24s %3d px, %d left over%s"
          % ('"%s"' % a.lines[0], used, TEXT_W - used,
             "  CUT OFF" if used > TEXT_W else ""))

    # The card number and the identification are two runs on the board, so
    # they are here too. On the command line they are one string split at
    # the first double space, which is where the board puts its own gap.
    info = a.lines[1]
    at = info.find("  ")
    left, right = (info[:at], info[at:].strip()) if at >= 0 else (info, "")
    used = draw_info(img, left, right, a.id_off, small)
    print("  info   %-24s %3d px, %d left over%s"
          % ('"%s" + "%s"' % (left, right), used, TEXT_W - used,
             "  CUT OFF" if used > TEXT_W else ""))

    badges = []
    for part in a.badges.split(","):
        part = part.strip()
        if not part:
            continue
        at = part.rfind(":")
        badges.append((part[:at], part[at + 1:] not in ("0", "off", "")))
    tiny = read_font(FONT12, "DISPFONT12")
    draw_badges(img, badges, tiny)
    total = sum(badge_width(t, tiny) for t, _ in badges)
    gaps = len(badges) - 1
    print("  badges %-28s %3d px of %d, %d px of slack over %d gaps%s"
          % (",".join(t for t, _ in badges), total, TEXT_W, TEXT_W - total, gaps,
             "  DOES NOT FIT" if total > TEXT_W else ""))

    Image.fromarray(img).save("%s/display.png" % a.out)
    Image.fromarray(img).resize((W * 4, H * 4), Image.NEAREST).save("%s/display_big.png" % a.out)
    print("display.png and display_big.png written to %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())

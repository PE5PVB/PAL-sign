"""Builds the PAL-sign app icon: the same eight 75% EBU colour bars the
board itself draws (CardPreview.cs's own Bars[]), so the icon is a
literal miniature of what the tool configures, not an invented mark.
"""
from PIL import Image, ImageDraw

BARS = [
    (255, 255, 255), (191, 191, 0), (0, 191, 191), (0, 191, 0),
    (191, 0, 191), (191, 0, 0), (0, 0, 191), (0, 0, 0),
]

OUT = "src/app.ico"
ACCENT = (0, 140, 255)

# Eight thin bars read as noise under about 40px; small sizes get a
# reduced, wider-barred version of the same pattern (still first,
# middle and last of the real eight, so it stays recognisably the same
# mark) instead of a literal downscale.
SIZES = [16, 24, 32, 48, 64, 128, 256]
FULL_BELOW = 40


def render(size):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    frame = max(1, round(size / 16))
    radius = max(2, size // 7)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius,
                         fill=(15, 18, 20, 255))

    bars = BARS if size >= FULL_BELOW else [BARS[0], BARS[2], BARS[4], BARS[6]]
    inset = max(2, round(size * 0.14))
    bar_area = [inset, inset, size - 1 - inset, size - 1 - inset]
    bw = (bar_area[2] - bar_area[0] + 1) / len(bars)
    for i, c in enumerate(bars):
        x0 = bar_area[0] + round(i * bw)
        x1 = bar_area[0] + round((i + 1) * bw) - 1
        d.rectangle([x0, bar_area[1], x1, bar_area[3]], fill=c + (255,))
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius,
                         outline=ACCENT + (255,), width=max(1, frame))
    return img


images = {s: render(s) for s in SIZES}

# Pillow's own ICO writer always resamples one base image for every
# requested size, silently ignoring append_images -- confirmed by
# inspecting the 48px frame it produced, which still had eight bars.
# The ICO format itself is simple enough (Vista+) to hold each size as
# its own PNG, so it is written by hand here instead.
import io
import struct

entries = []
for s in SIZES:
    buf = io.BytesIO()
    images[s].save(buf, format="PNG")
    entries.append((s, buf.getvalue()))

with open(OUT, "wb") as f:
    f.write(struct.pack("<HHH", 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for s, data in entries:
        wh = 0 if s >= 256 else s   # 0 means 256 in the ICO format itself
        f.write(struct.pack("<BBBBHHII", wh, wh, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    for _, data in entries:
        f.write(data)
print("wrote", OUT)

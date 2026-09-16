"""
Converts an ordinary image into a full screen picture in flash.

    python tools/convert_image.py photo.jpg myphoto "My photo"

produces src/testcards/myphoto_raw.inc, _raw.S and myphoto.cpp. After
that, two more lines by hand: PATTERN_MYPHOTO in pattern.h and in the
table in main.cpp.

    python tools/convert_image.py photo.jpg --port COM33 --slot 16

writes no files but sends the same converted data over the serial port
to one of the Custom slots. It ends up in a reserved part of flash and
survives a power cut; see src/photoflash.h. The same conversion, so the
same colour space, the same scaling and the same level limiting, and no
second implementation that can drift out of step.

Unlike the test cards this does NOT go through the run length encoding.
A photograph does not encode: nearly every pixel differs from its
neighbour. So it sits in flash unprocessed and is read straight out per
picture line. That costs 810 kB of flash and not a byte of RAM.

It needs 22.5 MB/s without a break (1440 bytes per 64 us), which is why
the QSPI clock divisor matters here. See CFG_FLASH_CLKDIV in config.h.

FLICKER FILTER. By default a photograph goes through a filter that takes
the interlace twitter out; see anti_twitter(). It costs vertical
sharpness, because that twitching component IS vertical detail. With
--flicker 0.5 you leave half of it in and with --flicker 0 nothing is
done about it.

Aspect ratio: the 720x576 raster is shown on a 4:3 screen, so the pixels
are slightly wider than they are tall. An image with square pixels is
converted for that and by default fits with black bars; with --fill it
is cropped instead. A source that is already exactly 720x576 passes
through one to one, since it was clearly made for this raster already.
With --square you can force it to be treated as square pixels after all.
"""

import argparse
import io
import os
import sys

import numpy as np

from asmtable import write_table
from PIL import Image

# Bump this on any change to the pixel arithmetic below (fit_in(),
# anti_twitter(), to_422()) and bump ImageConvert.AlgoVersion in
# PC Software/src/ImageConvert.cs to the same number in the same
# commit. compare.py checks the two match before it trusts its own
# byte-for-byte result.
ALGO_VERSION = 1

SCREEN_W, SCREEN_H = 720, 576
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src",
                       "testcards")

# On a 4:3 screen a picture pixel is 720/576 divided by 4/3 times as wide
# as it is tall.
PIXEL_ASPECT = (4.0 / 3.0) / (SCREEN_W / SCREEN_H)

FS = 13.5e6          # luma sampling
FSC = 4433618.75     # PAL colour subcarrier


def fit_in(im, fill, square):
    """Scale to 720x576 without mangling the aspect ratio."""
    if im.size == (SCREEN_W, SCREEN_H) and not square:
        # Already exactly the video raster: then this is not an image
        # with square pixels but a picture that is meant this way. Leave
        # it alone, otherwise it wrongly gets bars left and right.
        print("  already 720x576, passes through one to one")
        return im

    # From screen ratio to raster ratio: a raster pixel is wider than it
    # is tall, so fewer of them fit side by side. Divide, do not
    # multiply.
    source = im.width / im.height / PIXEL_ASPECT
    target = SCREEN_W / SCREEN_H
    if (source > target) != fill:
        w = SCREEN_W
        h = max(1, int(round(SCREEN_W / source)))
    else:
        h = SCREEN_H
        w = max(1, int(round(SCREEN_H * source)))
    im = im.resize((w, h), Image.LANCZOS)

    canvas = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
    canvas.paste(im, ((SCREEN_W - w) // 2, (SCREEN_H - h) // 2))
    return canvas


def notch_luma(y, width_hz, taps=41):
    """Take a narrow band around the colour subcarrier out of the luma.

    Cross colour comes about because fine brightness detail around
    4.43 MHz is partly read as colour by the decoder: on a tiled roof or
    on fabric you see that as coloured moire. Taking out exactly that
    band removes the cause.

    This is a good deal gentler than the notch filter in the encoder:
    that one is fixed and cuts broadly, while here you choose the width
    and the rest of the sharpness stays untouched. It also costs nothing
    while running, because it happens here and not on the Pico.
    """
    # Import scipy HERE and not at the top. The cross colour filter is
    # the only thing that needs it, and the configurator is bundled into
    # an .exe, where scipy would add tens of megabytes for a function
    # that is usually switched off.
    from scipy.signal import firwin

    lo = FSC - width_hz / 2
    hi = FSC + width_hz / 2
    h = firwin(taps, [lo, hi], pass_zero="bandstop", fs=FS)
    pad = taps // 2
    out = np.empty_like(y)
    for r in range(y.shape[0]):
        row = np.pad(y[r], pad, mode="edge")
        out[r] = np.convolve(row, h, "valid")
    return out


def anti_twitter(channel, strength):
    """Take the interlace twitter out of a channel.

    WHY THIS IS NEEDED. Even picture rows go to field 1 and odd ones to
    field 2 (see video.cpp). Anything that changes sign from row to row
    therefore sits differently in the one field than in the other, and
    that shows as flicker at 25 Hz. In the drawn test cards we solve that
    by giving thin horizontal bands an even number of rows, but a
    photograph cannot be laid out so neatly: the edge of a roof, a
    striped shirt or a horizontal branch produces exactly such an
    alternation.

    THE KERNEL IS DERIVED AND NOT CHOSEN. The difference between the
    fields on row y is d[y] = I[y] - (I[y-1]+I[y+1])/2. For a signal that
    alternates per row, I[y] = A(-1)^y, that works out to d[y] = 2*I[y];
    check it. Taking half of d off therefore wipes that component out
    exactly:

        I[y] - d[y]/2 = I[y]/2 + (I[y-1]+I[y+1])/4

    and that is the kernel [1,2,1]/4. Its response at the alternating
    frequency is exactly 0; at half of that still 0.655 and at a fifth
    still 0.905. So it is not a general softening but a targeted damping
    of precisely the thing that blinks.

    WHAT IT COSTS. Vertical sharpness, and that is unavoidable: the
    twitching component IS vertical detail. `strength` sets the balance:
    1.0 takes it out completely, 0.0 leaves the photograph alone. In
    between it is mixed linearly, so 0.5 leaves half of the alternating
    component in.

    The edge rows are repeated rather than counted as black; otherwise
    the top and bottom picture rows get a dark hem.
    """
    if strength <= 0:
        return channel
    out = np.empty_like(channel)
    above = np.vstack([channel[:1], channel[:-1]])
    below = np.vstack([channel[1:], channel[-1:]])
    smooth = 0.5 * channel + 0.25 * above + 0.25 * below
    out[:] = channel + strength * (smooth - channel)
    return out


def to_422(rgb, notch_hz=0.0, flicker=1.0):
    """sRGB to 4:2:2 YCbCr at studio levels (Y 16..235, chroma 16..240)."""
    r, g, b = (rgb[..., i].astype(np.float64) / 255.0 for i in range(3))
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = 128 + 112 * (b - y) / 0.886
    cr = 128 + 112 * (r - y) / 0.701
    y = 16 + 219 * y
    if notch_hz > 0:
        y = notch_luma(y, notch_hz)

    # Take the twitter out BEFORE anything is rounded or limited, and on
    # all three channels: in 4:2:2 chroma is halved horizontally but it
    # is complete vertically, so a colour edge can blink just as well as
    # a brightness edge.
    y = anti_twitter(y, flicker)
    cb = anti_twitter(cb, flicker)
    cr = anti_twitter(cr, flicker)

    # Halve the chroma by averaging pairs, not by throwing one of them
    # away: that saves ragged colour edges.
    cb = (cb[:, 0::2] + cb[:, 1::2]) / 2
    cr = (cr[:, 0::2] + cr[:, 1::2]) / 2

    lines = np.zeros((SCREEN_H, SCREEN_W * 2), np.uint8)
    lines[:, 1::2] = np.clip(np.round(y), 16, 235)
    lines[:, 0::4] = np.clip(np.round(cb), 16, 240)
    lines[:, 2::4] = np.clip(np.round(cr), 16, 240)
    return lines


SECTOR = 4096


# The words an error message from the board is recognised by. They belong
# to the TEXT of the firmware and not to the protocol, so if the wording
# changes there, this list goes with it. That has gone wrong once.
ERROR_WORDS = (b"failed", b"aborted", b"too small", b"does not exist")


def wait_for(s, what, seconds=30):
    """Read lines until one starts with `what`. Error messages abort."""
    import time
    deadline = time.time() + seconds
    while time.time() < deadline:
        line = s.readline()
        if not line:
            continue
        if line.startswith(what):
            return line
        for w in ERROR_WORDS:
            if w in line:
                raise UploadError(line.decode("ascii", "replace").strip())
    raise UploadError("no answer to %r within %d s" % (what, seconds))


class UploadError(Exception):
    """Something went wrong during the upload.

    A type of its own and not a SystemExit: the configurator wants to
    show the message in a window instead of terminating the program.
    """


def upload(port, lines, name, slot, report=None, progress=None):
    """Send the converted picture data to a Custom slot over the serial port.

    The protocol is described in main.cpp at fotoUpload(). The heart of
    it: the Pico acknowledges every sector with "A<n>" and only then may
    the next one follow. That is not a courtesy but a necessity: while
    writing, the interrupts are off and the USB is not serviced, so
    sending on would overflow that buffer.

    There is NO video during an upload: the firmware takes over the loop
    and parks the other core. The board does NOT restart and the port
    simply stays open.

    `report(text)` gets every progress line and `progress(n, total)`
    every acknowledged sector. Without those two everything goes to the
    console, so that this script keeps doing from the command line what
    it always did. The configurator hangs its own window on it; see
    PC Software/.
    """
    if report is None:
        report = print
    import zlib

    try:
        import serial
        import serial.tools.list_ports
    except ImportError:
        raise UploadError("pyserial is missing. Install it in the SAME python you "
                          "are running this with:\n"
                          "    python -m pip install pyserial\n"
                          "(the python of PlatformIO does have pyserial, but then it has no "
                          "numpy/PIL/scipy, so it cannot run this script)")

    ports = [p.device for p in serial.tools.list_ports.comports()]
    if ports and port not in ports:
        raise UploadError("port %s does not exist. Found: %s" % (port, ", ".join(ports)))

    data = lines.tobytes()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    blocks = (len(data) + SECTOR - 1) // SECTOR
    report("upload to slot %d (Custom %d): %d bytes, %d sectors, crc 0x%08X"
           % (slot, slot, len(data), blocks, crc))

    # Not in a `with`: closing must not trip over a port that, for
    # whatever reason, is already gone.
    s = serial.Serial(port, 115200, timeout=10)
    try:
        s.reset_input_buffer()
        s.write(b"u")
        line = wait_for(s, b"[upload] ready ")
        expected = int(line.split()[2])
        if expected != len(data):
            raise UploadError("the firmware expects %d bytes, we have %d"
                              % (expected, len(data)))

        # The line starts with the slot number, 1 through 16. The board
        # reads TWO digits, so "16 photo.jpg" ends up in slot 16 and not
        # in slot 1 with a file whose name starts with a 6.
        header = ("%d " % slot).encode("ascii") + name.encode("ascii", "replace")[:30]
        s.write(header + b"\n")
        report("erasing the slot; that takes ten seconds or so")
        # ERASING IS THE LONG STEP: 204 sectors, and the W25Q128JV gives
        # 45 ms typical and 400 ms maximum per sector of 4 kB. That is
        # 9 to 82 seconds, so the default 30 was too short.
        wait_for(s, b"[upload] erased", 180)
        report("slot erased, sending...")

        for i in range(blocks):
            s.write(data[i * SECTOR:(i + 1) * SECTOR])
            s.flush()
            wait_for(s, b"A%d" % i)
            if i % 20 == 0 or i + 1 == blocks:
                print("  %d/%d" % (i + 1, blocks), end="\r", flush=True)

        answer = wait_for(s, b"[upload] done,").decode("ascii", "replace").strip()
        if progress is None:
            print()
        report(answer)
        # The firmware reports the CRC it has read back out of the FLASH,
        # not the CRC of what we sent. If the two agree, then it really
        # is in there correctly.
        if ("0x%08X" % crc).lower() not in answer.lower():
            raise UploadError("the CRC of the firmware differs from ours, do not trust it")
        report("CRC agrees; the photograph is in flash and survives a power cut.")
    finally:
        try:
            s.close()
        except Exception:
            pass
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("image")
    p.add_argument("name", nargs="?", default="upload",
                   help="lower case, becomes the file name and the symbol")
    p.add_argument("title", nargs="?", help="what appears in the serial list")
    p.add_argument("--fill", action="store_true",
                   help="crop until the picture is filled, instead of black bars")
    p.add_argument("--square", action="store_true",
                   help="treat a 720x576 source as square pixels after all")
    p.add_argument("--port", metavar="COMx",
                   help="do not write a file but send to the board over the serial port")
    p.add_argument("--slot", type=int, choices=range(1, 17), default=1, metavar="1..16",
                   help="which uploadable test card: 1 = Custom 1 (default) through 16")
    p.add_argument("--flicker", type=float, default=1.0, metavar="STRENGTH",
                   help="0 through 1: how much of the interlace twitter is taken out "
                        "(default 1, so all of it; costs vertical sharpness)")
    p.add_argument("--notch", type=float, default=0.0, metavar="MHZ",
                   help="take a band of this width around 4.43 MHz out of the luma, "
                        "against cross colour on fine detail (try 1.0)")
    a = p.parse_args()
    title = a.title or a.name

    im = Image.open(a.image).convert("RGB")
    print("%s: %dx%d" % (a.image, im.width, im.height))
    canvas = fit_in(im, a.fill, a.square)
    lines = to_422(np.array(canvas), a.notch * 1e6, a.flicker)

    if a.port:
        try:
            return upload(a.port, lines, os.path.basename(a.image), a.slot)
        except UploadError as e:
            raise SystemExit(str(e))

    nl = chr(10)
    write_table("%s/%s_raw.inc.h" % (OUT_DIR, a.name), lines.tobytes())
    print("  src/testcards/%s_raw.inc.h: %d bytes of table" % (a.name, lines.size))

    with io.open("%s/%s_raw.S" % (OUT_DIR, a.name), "w", encoding="utf-8", newline=nl) as fh:
        fh.write("""// GENERATED by tools/convert_image.py -- do not edit by hand.
//
// An unprocessed picture in flash: %d rows of %d bytes, 4:2:2.
.section .rodata
.balign 4
.global %s_raw
%s_raw:
#include "%s_raw.inc.h"
""" % (SCREEN_H, SCREEN_W * 2, a.name, a.name, a.name))
        # The size as well, so that this file changes content when the
        # data changes. Without that the build environment links the old
        # object; see the explanation in convert_rom_pattern.py.
        fh.write("// %s_raw.inc.h: %d bytes%s" % (a.name, lines.size, chr(10)))

    with io.open("%s/%s.cpp" % (OUT_DIR, a.name), "w", encoding="utf-8", newline=nl) as fh:
        fh.write("""// GENERATED by tools/convert_image.py -- source: %s
//
// A full screen image, unprocessed in flash: %d rows of %d bytes. No run
// length encoding and no palette, because there is nothing to be gained
// from those on a photograph. So it costs 810 kB of flash and not a byte
// of RAM.
//
// Per picture line 1440 bytes are copied out of flash. That needs
// 22.5 MB/s without a break, which is what sets CFG_FLASH_CLKDIV in
// config.h.
#include <string.h>

#include "gfx.h"
#include "pattern.h"

extern "C" {
extern const uint8_t %s_raw[];
}

static void %sInit() {}

static void %sRenderRow(uint8_t *base, int y) {
  memcpy(base, %s_raw + (unsigned)y * (SCREEN_W * 2), SCREEN_W * 2);
}

const Pattern PATTERN_%s = {"%s", %sInit, %sRenderRow, nullptr};
""" % (os.path.basename(a.image), SCREEN_H, SCREEN_W * 2, a.name, a.name, a.name, a.name,
       a.name.upper(), title, a.name, a.name))

    print("  src/testcards/%s_raw.S and src/testcards/%s.cpp written" % (a.name, a.name))
    print()
    print("Still to be added:")
    print("  pattern.h : extern const Pattern PATTERN_%s;" % a.name.upper())
    print("  main.cpp  : &PATTERN_%s in the PATTERNS table" % a.name.upper())


if __name__ == "__main__":
    sys.exit(main())

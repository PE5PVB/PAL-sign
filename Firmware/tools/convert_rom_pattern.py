"""
Converts EPROM dumps of Philips test card generators into the form the
firmware plays back.

Source: PhilipsPatternRom/render/<set>, dumps of real generators that
have already been converted to BT.601 levels.

SETS is at the bottom: that is where you add a test card. Every set
yields five .inc.h files plus a .h and a .S in src/, all of them with the
name of the set in front, so that several test cards can exist side by
side. The firmware bakes them all into flash but copies only the active
picture to RAM.

A set can hold several variants of the same picture (on the PM5644 G00
those are: with empty insert boxes, and with the grid running through).
Variants share nearly all picture lines, so those are deduplicated
across the whole set; only the line index differs.

Every unique line is stored as run length encoding (RLE) over 32-bit
words. A word is exactly one pixel pair in 4:2:2 (Cb,Y,Cr,Y), so
decoding is a simple loop of word stores. The values themselves go
through a palette, because only a few hundred different pixel pairs
occur; that keeps the table small enough to fit in RAM. That is not a
luxury but a necessity: decoding from flash turned out to be slower
than drawing the pattern.
"""

import io
import json
import os
import sys

import numpy as np

from asmtable import write_table

ROM_BASE = r"c:/Users/pe5pv/OneDrive - Vereniging van Radio Zendamateurs/GitHub/PhilipsPatternRom/render"
HACKTV_BASE = r"c:/Users/pe5pv/OneDrive - Vereniging van Radio Zendamateurs/GitHub/hacktv-testsignals"
TIFF_BASE = r"c:/Users/pe5pv/OneDrive - Vereniging van Radio Zendamateurs/GitHub/bbc-testcards-out"
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src",
                       "testcards")

SCREEN_W = 720
ACTIVE_ROWS = 576

# The old generation (G00) samples luma at exactly 13.5 MHz, precisely
# the raster that BT.656 and the ADV7391 expect. Those samples pass
# through one to one, without resampling and so without loss of
# sharpness.
#
# Later generations sample faster (the G913 has 1045 active samples
# instead of 707) and do have to be resampled. That is done with an
# area average: every output sample is the weighted average of the
# input samples it overlaps. That way filtering happens while shrinking,
# so no aliasing and no overshoot.
ACTIVE_SAMPLES = 707

MAX_RUN = 255  # so that a run length fits in a byte

# levels: only of interest when resampling. Averaging turns every edge
# into a small gradient and so every edge into a handful of separate
# runs; that is what drains the table (332 KB instead of 156). By
# bringing the result back to the N most common luma levels those
# gradients coincide again. The commonly used levels are exactly those
# of the flat areas, so those stay exact: only the transitions get
# rounded. With 64 levels the deviation in flat areas is zero and none
# of it is visible.
SETS = {
    "pm5644g00": {
        "title": "PM5644 (G00)",
        # Chroma leads luma in this set; see shift_chroma(). The value was
        # found by shifting and measuring again until the skew matched the
        # cards that are drawn: it leaves 1.5 ns of the original 105.
        "chroma_shift": 1.412,
        "variants": ["PM5644G00_pat0", "PM5644G00_pat1", "PM5644G00_pat2"],
        "note": ("variant 0 = both insert boxes (date and time), "
                        "1 = the right one only (time), 2 = neither, grid running through"),
    },
    "pm5644g924": {
        "title": "PM5644 (G924)",
        # Idem, 212 ns of chroma lead, 0.3 ns left over.
        "chroma_shift": 2.862,
        "variants": ["PM5644G924_pat0", "PM5644G924_pat1", "PM5644G924_pat2",
                      "PM5644G924_pat3"],
        "note": ("0 = date and time, 1 = time only, 2 = neither; "
                        "3 = as 0 but without the chroma test bars"),
        "levels": 64,
    },
    # These two do not come from an EPROM but from hacktv-testsignals:
    # composite baseband, so they have to be demodulated first. See
    # paldec.py. Their edges are band limited instead of hard as a
    # result, which gives more intermediate levels; hence the reduction
    # here as well.
    # Test Card G is NOT in this table: it is derived from the PM5644
    # G00 tables by tools/make_testcardg.py. Run that script instead.
    # Not from a source but computed: see build_frame_multiburst().
    "multiburst": {
        "title": "Multiburst",
        "source": "computed",
        "note": "test signal: white reference plus six frequency packets",
    },
    "fubk4x3": {
        "title": "FUBK 4:3",
        "source": "hacktv",
        "file": "fubk_4x3.bin",
        "note": "German FUBK, demodulated from composite",
        "max_error": 1,
        "levels": 64,
        "levels_chroma": 32,
    },
    "fubk16x9": {
        "title": "FUBK 16:9",
        "source": "hacktv",
        "file": "fubk_16x9_pal.bin",
        "note": "FUBK in 16:9 (anamorphic), demodulated from composite",
        "max_error": 1,
        "levels": 64,
        "levels_chroma": 32,
    },
    "pulsebar": {
        "title": "Pulse and bar",
        "source": "hacktv",
        "file": "pulse_bar_pal.bin",
        "note": "test signal, monochrome",
        "levels": 128,
        "levels_chroma": 0,
        "mono": True,
    },
    "sinxx": {
        "title": "sin(x)/x",
        "source": "hacktv",
        "file": "sin_x_x_pal.bin",
        "note": "test signal, monochrome",
        "levels": 128,
        "levels_chroma": 0,
        "mono": True,
    },
    "pm5644g913": {
        "title": "PM5644 (G913)",
        "variants": ["PM5644G913_pat0"],
        "note": "monoscope with the Indian head; monochrome, no text boxes",
        "levels": 64,
    },
}


def area_resample(src, n_out):
    """Area average along the last axis: every output sample is the
    weighted average of the input samples it overlaps."""
    n_in = src.shape[-1]
    edges = np.linspace(0, n_in, n_out + 1)
    cs = np.concatenate([np.zeros(src.shape[:-1] + (1,)), np.cumsum(src.astype(np.float64), -1)], -1)

    def op(p):
        i = np.clip(np.floor(p).astype(int), 0, n_in)
        f = p - np.floor(p)
        lo = np.take(cs, i, -1)
        hi = np.take(cs, np.clip(i + 1, 0, n_in), -1)
        return lo + (hi - lo) * f

    return (op(edges[1:]) - op(edges[:-1])) / np.diff(edges)


def reduce_levels(a, n_levels, max_error=None):
    """Reduce a to the `n_levels` most common levels, moving every sample to
    the nearest level that is kept.

    max_error PUTS A CEILING ON THE ERROR. Without it, keeping only the most
    common levels wrecks a smooth ramp: every value in a gradient occurs
    about equally rarely, so hardly any of them survive and the ramp comes
    out as a staircase. Measured on the two gradient bars at the bottom of
    the FUBK card, chroma moved by up to 25 counts and the plateaus grew
    to 46 samples wide. With max_error set, the most common levels are kept
    as before and then whatever else is needed so that no sample has to
    move further than max_error. The flat areas still collapse onto one
    level, so the table stays compact, but the ramps survive.

    Set it on any source that holds a real gradient. Leave it off where
    the picture is flat patches and edges only: there the extra levels buy
    nothing and cost a lot. On the PM5644 EPROM sets, which are resampled
    from 1045 to 707 samples and therefore have a unique intermediate
    value at nearly every edge, max_error=1 pushes the palette from 3052 to
    6239 entries without a single gradient to show for it.
    """
    values, counts = np.unique(np.round(a).astype(np.uint8), return_counts=True)
    if len(values) <= n_levels:
        return np.round(a)
    keep = sorted(values[np.argsort(-counts)[:n_levels]].tolist())
    if max_error is not None:
        for v in values.tolist():
            if np.abs(np.asarray(keep) - v).min() > max_error:
                keep.append(v)
                keep.sort()
    keep = np.asarray(keep, np.float64)
    i = np.clip(np.searchsorted(keep, a), 1, len(keep) - 1)
    return np.where(np.abs(a - keep[i - 1]) <= np.abs(a - keep[i]), keep[i - 1], keep[i])


# Multiburst: what you measure the amplitude response of the chain with.
# On the left a black and a white reference to calibrate the amplitude
# against, then six packets of sine of equal amplitude and rising
# frequency. On a good chain all six are the same height; if the last
# packet sags, you know exactly where the bandwidth stops.
MULTIBURST_MHZ = (0.5, 1.0, 2.0, 3.0, 4.0, 4.8)


def build_frame_multiburst():
    """Compute the multiburst signal; monochrome."""
    fs = 13.5e6
    y = np.full(SCREEN_W, 126.0)  # rest level, halfway between black and white

    # Calibration blocks: 0..47 black, 48..95 white. With those you can
    # fix the vertical scale on the scope before measuring the packets.
    y[0:48] = 16
    y[48:96] = 235

    # Six packets of 96 px with 4 px of rest in between, from x=112. That
    # runs to 708 and so fits within the 720.
    amp = 66.0  # 60% of the range, so 60..192: stays within 16..235
    width, gap, x0 = 96, 4, 112
    assert x0 + len(MULTIBURST_MHZ) * width + (len(MULTIBURST_MHZ) - 1) * gap <= SCREEN_W
    for mhz in MULTIBURST_MHZ:
        n = np.arange(width)
        y[x0:x0 + width] = 126 + amp * np.sin(2 * np.pi * mhz * 1e6 * n / fs)
        x0 += width + gap

    Y = np.clip(np.round(y), 16, 235).astype(np.uint8)

    lines = np.zeros((ACTIVE_ROWS, SCREEN_W * 2), np.uint8)
    lines[:, 1::2] = Y[None, :]
    lines[:, 0::4] = 128  # monochrome
    lines[:, 2::4] = 128
    return lines, 0


def build_frame_tiff(filename, levels, levels_chroma, max_error=None):
    """Read an R'G'B' TIFF from the bbc-testcards build pipeline to 4:2:2.

    The values run from 0 to 1 but are allowed to fall outside that: the
    card is designed in Y'UV and some saturated colours do not fit in
    the RGB cube. So do not clip, just convert back.
    """
    import tifffile

    rgb = tifffile.imread(os.path.join(TIFF_BASE, filename)).astype(np.float64)
    R, G, B = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    Yf = 0.299 * R + 0.587 * G + 0.114 * B
    CBf = 128 + 112 * (B - Yf) / 0.886
    CRf = 128 + 112 * (R - Yf) / 0.701
    Yf = 16 + 219 * Yf

    # Chroma to 4:2:2 by averaging pairs, not by throwing one of them
    # away: that saves aliasing on the colour transitions.
    CBf = (CBf[:, 0::2] + CBf[:, 1::2]) / 2
    CRf = (CRf[:, 0::2] + CRf[:, 1::2]) / 2

    Y = np.clip(np.round(reduce_levels(Yf, levels, max_error)), 16, 235).astype(np.uint8)
    CB = np.clip(np.round(reduce_levels(CBf, levels_chroma, max_error)), 16, 240).astype(np.uint8)
    CR = np.clip(np.round(reduce_levels(CRf, levels_chroma, max_error)), 16, 240).astype(np.uint8)

    lines = np.zeros((ACTIVE_ROWS, SCREEN_W * 2), np.uint8)
    lines[:, 1::2] = Y
    lines[:, 0::4] = CB
    lines[:, 2::4] = CR
    return lines, 0


def build_frame_hacktv(filename, levels, levels_chroma, mono=False, max_error=None):
    """Demodulate a composite test signal from hacktv to 4:2:2."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import paldec

    Y, CB, CR = paldec.frame576(os.path.join(HACKTV_BASE, filename))
    # One picture line of shift: in the source the fields lie one line
    # differently than in our counting. Measured by laying the Philips
    # signal from this same collection against the EPROM version.
    Y, CB, CR = (np.roll(a, -1, axis=0) for a in (Y, CB, CR))

    Y = reduce_levels(Y.astype(np.float64), levels, max_error)
    if mono:
        # Black and white signal: chroma should be exactly neutral. What
        # the demodulator yields there is only residue of the
        # separation, and that would blow up the table for nothing.
        CB = np.full(CB.shape, 128.0)
        CR = np.full(CR.shape, 128.0)
    else:
        CB = reduce_levels(CB.astype(np.float64), levels_chroma, max_error)
        CR = reduce_levels(CR.astype(np.float64), levels_chroma, max_error)
    Y = np.clip(np.round(Y), 16, 235).astype(np.uint8)
    CB = np.clip(np.round(CB), 16, 240).astype(np.uint8)
    CR = np.clip(np.round(CR), 16, 240).astype(np.uint8)

    lines = np.zeros((ACTIVE_ROWS, SCREEN_W * 2), np.uint8)
    lines[:, 1::2] = Y
    lines[:, 0::4] = CB[:, 0::2]
    lines[:, 2::4] = CR[:, 0::2]
    return lines, 0


def shift_chroma(C, dx):
    """Move a chroma plane dx CHROMA samples to the right.

    WHY THIS EXISTS. In the EPROM sets the chroma comes out ahead of the
    luma: measured on the finished tables by cross correlating the luma
    transitions with the chroma ones, 2.02 samples on the G00 and 3.06 on
    the G924, against 0.50 for every card that is drawn or demodulated.
    That 0.50 is the 4:2:2 convention itself -- chroma sample k covers
    luma 2k and 2k+1 -- so the real error is 1.5 and 2.6 luma samples,
    which is 113 and 190 ns of chroma leading luma. On a vectorscope that
    is plain to see, and it is why those two cards needed the encoder's
    chroma delay set to 1 while no other card did.

    WINDOWED SINC AND NOT LINEAR. The offsets are not whole samples, so
    something has to interpolate. Averaging two neighbours is a 2 tap
    filter: at the 6.75 MHz chroma rate that is -1.70 dB at 1.3 MHz,
    right in the band the encoder passes, and it would show as softer
    colour edges. Lanczos-3 is +0.20 dB there, so flat.
    """
    if dx == 0:
        return C
    a = 3
    n = C.shape[1]
    whole = int(np.floor(dx))
    frac = dx - whole
    k = np.arange(-a + 1, a + 1)
    tap = np.sinc(k - frac) * np.sinc((k - frac) / a)
    tap /= tap.sum()
    idx = np.arange(n)
    out = np.zeros(C.shape, np.float64)
    for t, w in zip(k, tap):
        out += w * C[:, np.clip(idx - whole - t, 0, n - 1)]
    return out


def build_frame(sub, levels, chroma_shift=0.0):
    """Read a variant and build 576 BT.656 rows of 1440 bytes from it."""
    meta = json.load(open("%s/%s/meta.json" % (ROM_BASE, sub)))
    w, h = meta["luma"]["width"], meta["luma"]["height"]
    y = np.fromfile("%s/%s/Y10_bt601.bin" % (ROM_BASE, sub), dtype="<u2").reshape(h, w)
    cb = np.fromfile("%s/%s/Cb_bt601.bin" % (ROM_BASE, sub), dtype=np.uint8).reshape(h, w // 2)
    cr = np.fromfile("%s/%s/Cr_bt601.bin" % (ROM_BASE, sub), dtype=np.uint8).reshape(h, w // 2)

    aw = meta["active_window_luma"]
    x0, aw_w = aw["x"], aw["width"]

    # 10-bit (64..940) -> 8-bit (16..235); the ADV7391 runs in 8-bit here.
    Y = (y[:ACTIVE_ROWS, x0:x0 + aw_w] >> 2).astype(np.float64)
    CB = cb[:ACTIVE_ROWS, x0 // 2:(x0 + aw_w) // 2].astype(np.float64)
    CR = cr[:ACTIVE_ROWS, x0 // 2:(x0 + aw_w) // 2].astype(np.float64)

    if aw_w != ACTIVE_SAMPLES:
        print("  %s: %d active samples -> %d, reduced to %d luma levels"
              % (sub, aw_w, ACTIVE_SAMPLES, levels))
        Y = reduce_levels(area_resample(Y, ACTIVE_SAMPLES), levels)
        CB = area_resample(CB, ACTIVE_SAMPLES // 2)
        CR = area_resample(CR, ACTIVE_SAMPLES // 2)
        aw_w = ACTIVE_SAMPLES

    # AFTER any resampling and BEFORE the rounding, so the interpolation
    # does not work on values that have already been quantised. The
    # figure is in LUMA samples, hence the halving: chroma runs at half
    # the rate. See shift_chroma() for where the numbers come from.
    if chroma_shift:
        CB = shift_chroma(CB, chroma_shift / 2.0)
        CR = shift_chroma(CR, chroma_shift / 2.0)

    Y = np.clip(np.round(Y), 16, 235).astype(np.uint8)
    CB = np.clip(np.round(CB), 16, 240).astype(np.uint8)
    CR = np.clip(np.round(CR), 16, 240).astype(np.uint8)

    # The original has 707 active samples, BT.656 carries 720. Centre it
    # and leave the rest black; on a CRT that falls in the overscan.
    pad = (SCREEN_W - aw_w) // 2
    pad -= pad & 1  # start on an even pixel, otherwise the chroma phase is wrong

    lines = np.zeros((ACTIVE_ROWS, SCREEN_W * 2), np.uint8)
    lines[:, 0::4] = 128  # Cb neutral
    lines[:, 2::4] = 128  # Cr neutral
    lines[:, 1::4] = 16   # Y black
    lines[:, 3::4] = 16
    for i in range(aw_w):
        lines[:, (pad + i) * 2 + 1] = Y[:, i]
    for i in range(CB.shape[1]):
        px = pad + i * 2
        lines[:, px * 2 + 0] = CB[:, i]
        lines[:, px * 2 + 2] = CR[:, i]
    return lines, pad


def convert(name, cfg):
    print("%s (%s)" % (name, cfg["title"]))

    frames, pad = [], None
    if cfg.get("source") == "computed":
        f, pad = build_frame_multiburst()
        frames.append(f)
        print("  computed: %s" % ", ".join("%g MHz" % m for m in MULTIBURST_MHZ))
    elif cfg.get("source") == "tiff":
        for b in cfg["files"]:
            f, pad = build_frame_tiff(b, cfg["levels"], cfg["levels_chroma"],
                                      cfg.get("max_error"))
            frames.append(f)
            print("  %s: read" % b)
    elif cfg.get("source") == "hacktv":
        f, pad = build_frame_hacktv(cfg["file"], cfg["levels"], cfg["levels_chroma"],
                                    cfg.get("mono", False), cfg.get("max_error"))
        frames.append(f)
        print("  %s: demodulated from composite" % cfg["file"])
    else:
        for sub in cfg["variants"]:
            f, pad = build_frame(sub, cfg.get("levels", 0), cfg.get("chroma_shift", 0.0))
            frames.append(f)
            print("  %s: read" % sub)

    encode_and_write(name, cfg, frames, pad)


def encode_and_write(name, cfg, frames, pad):
    """Deduplicate, encode with RLE and write the files out.

    Separate from convert() because make_testcardf.py builds its picture
    itself and only has to pass through here.
    """
    # Deduplicate across all variants.
    uniq = {}
    rows = []
    for f in frames:
        idx = []
        for r in range(ACTIVE_ROWS):
            key = f[r].tobytes()
            if key not in uniq:
                uniq[key] = len(uniq)
            idx.append(uniq[key])
        rows.append(idx)
    print("  unique picture lines: %d (of %d)" % (len(uniq), len(frames) * ACTIVE_ROWS))

    palette, palette_of = [], {}
    run_idx, run_len, start = [], [], [0]
    for key in uniq.keys():
        words = np.frombuffer(key, dtype="<u4")
        i = 0
        while i < len(words):
            j = i
            while j + 1 < len(words) and words[j + 1] == words[i]:
                j += 1
            v = int(words[i])
            if v not in palette_of:
                palette_of[v] = len(palette)
                palette.append(v)
            n = j - i + 1
            while n > 0:  # split long runs
                take = min(n, MAX_RUN)
                run_idx.append(palette_of[v])
                run_len.append(take)
                n -= take
            i = j + 1
        start.append(len(run_idx))
    print("  RLE runs: %d (on average %d per line)" % (len(run_idx), len(run_idx) / len(uniq)))
    print("  palette: %d unique pixel pairs" % len(palette))
    if len(palette) > 65535:
        raise SystemExit("palette does not fit in a 16-bit index")

    if not os.path.exists(OUT_DIR):
        os.makedirs(OUT_DIR)
    write_table("%s/%s_pal.inc.h" % (OUT_DIR, name), np.array(palette, dtype="<u4").tobytes())
    write_table("%s/%s_idx.inc.h" % (OUT_DIR, name), np.array(run_idx, dtype="<u2").tobytes())
    write_table("%s/%s_len.inc.h" % (OUT_DIR, name), np.array(run_len, dtype=np.uint8).tobytes())
    write_table("%s/%s_start.inc.h" % (OUT_DIR, name), np.array(start, dtype="<u4").tobytes())
    write_table("%s/%s_line.inc.h" % (OUT_DIR, name), np.array(rows, dtype="<u2").tobytes())

    total = (len(palette) * 4 + len(run_idx) * 2 + len(run_len) + len(start) * 4 +
             len(rows) * ACTIVE_ROWS * 2)
    ram = total - (len(rows) - 1) * ACTIVE_ROWS * 2  # only one variant in RAM
    print("  %d KB in flash, %d KB in RAM when this picture is active" % (total / 1024, ram / 1024))

    write_header(name, cfg, len(uniq), len(run_idx), len(palette), len(rows), pad)
    write_asm(name)
    print("  written: src/testcards/%s_data.h, src/testcards/%s_data.S and 5 .inc.h files" % (name, name))


def write_header(name, cfg, unique, runs, palette, variants, pad):
    P = name.upper()
    with io.open("%s/%s_data.h" % (OUT_DIR, name), "w", encoding="utf-8", newline=chr(10)) as fh:
        fh.write("""// GENERATED by tools/convert_rom_pattern.py -- do not edit by hand.
//
// %s
// Variants: %s
//   %s
#pragma once

#include <stdint.h>

static const int %s_UNIQUE_LINES = %d;
static const int %s_RUN_COUNT = %d;
static const int %s_PAL_SIZE = %d;

// The picture sits centred in the 720 samples that BT.656 carries, with
// %d black samples in front of it on every line. Written down here as a
// fact about the tables; the renderer does not need it, because the
// black is already in the run lengths.

// These tables live in flash. The renderer does NOT read them directly:
// rompattern.cpp copies the active picture to RAM at startup, because
// pulling separate streams out of flash per line costs more time than
// drawing the whole pattern (XIP cache misses).
extern "C" {
extern const uint32_t %s_pal[];    // palette: one pixel pair per entry
extern const uint16_t %s_idx[];    // palette index per run
extern const uint8_t %s_len[];     // repeat count per run (1..%d)
extern const uint32_t %s_start[];  // first run per unique line (plus a closing value)
extern const uint16_t %s_line[];   // [variant][row] -> unique line
}
""" % (cfg["title"],
       ", ".join(cfg.get("variants", cfg.get("files", [cfg.get("file", "-")]))),
       cfg["note"],
       P, unique, P, runs, P, palette,
       pad,
       name, name, name, MAX_RUN, name, name))


def write_asm(name):
    with io.open("%s/%s_data.S" % (OUT_DIR, name), "w", encoding="utf-8", newline=chr(10)) as fh:
        fh.write("""// GENERATED by tools/convert_rom_pattern.py -- do not edit by hand.
//
// Places the tables in flash, one section per table so that every
// alignment is right (uint32 on 4 bytes, uint16 on 2). rompattern.cpp
// copies them to RAM at startup; nothing reads from here while running.
.section .rodata

.balign 4
.global %s_pal
%s_pal:
#include "%s_pal.inc.h"

.balign 4
.global %s_start
%s_start:
#include "%s_start.inc.h"

.balign 2
.global %s_idx
%s_idx:
#include "%s_idx.inc.h"

.balign 2
.global %s_line
%s_line:
#include "%s_line.inc.h"

.balign 1
.global %s_len
%s_len:
#include "%s_len.inc.h"
""" % ((name,) * 15))
        # The sizes are written out on purpose, as a trailing comment: the
        # build system tracks this file's own content, not the separate
        # .incbin files, so without a content change here the stale object
        # stays linked after the data is regenerated, with a header
        # promising counts that no longer match what is actually in flash.
        for part in ("pal", "idx", "len", "start", "line"):
            fh.write("// %s_%s.inc.h: %d bytes%s"
                     % (name, part, os.path.getsize("%s/%s_%s.inc.h" % (OUT_DIR, name, part)),
                        chr(10)))


def main():
    requested = sys.argv[1:] or sorted(SETS.keys())
    for name in requested:
        if name not in SETS:
            raise SystemExit("unknown set %r; known are: %s" % (name, ", ".join(sorted(SETS))))
        convert(name, SETS[name])


if __name__ == "__main__":
    main()

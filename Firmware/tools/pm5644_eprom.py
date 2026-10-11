"""
Plays a PM5644 G00 or G924 vector table straight from the EPROM dumps,
field by field, the way the generator does, and turns the result into
BT.656 rows. The G00 part is described first; the G924 part follows at
G924 below.

Source: the ten G00 EPROMs of inaxeon/PTV_Preservation
(PM5644/PM5644G00/ROM_Dumps, each checksum equal to its file name).
Layout from inaxeon/PhilipsPatternRom (RomManager.cs, Utility.cs,
PatternRenderer.cs):
  - luma: four EPROMs interleaved, 8 MSBs each, plus a fifth with the two
    LSBs of all four (10 bit, inverted: sync 964, blanking 724, white 164);
  - R-Y and B-Y: two EPROMs each, interleaved (8 bit about 128, inverted,
    PAL-weighted V and U);
  - CPU EPROM: one vector table per pattern at 0x52F6 + n x 0xD98, three
    bytes a line, 1160 lines: four fields of 290, played A B C D. A line
    is a back porch sprite (64 units), the centre (120) and a front porch
    sprite (32); a unit is 4 luma or 2 chroma samples, so a line is 864
    luma samples at 13.5 MHz from 0H, sync included.
  - PhilipsPatternRom's ApplySourceFixes() patches are NOT applied: this
    is the EPROM as dumped.

Rows, laid against the luma of the earlier render: vector i of field A
is row 2i - 1 (BT.656 field 2), of field B row 2i - 2 (field 1). Two
vectors are the half lines instead: A0 has picture only after 43.7 us
(line 23, row 0) and B1 picture up to 32.2 us and an equalising pulse
at 32.4 us (line 623, row 575), where PhilipsPatternRom puts them too.
"""
import os

import numpy as np

EPROM_BASE = (r"c:/Users/pe5pv/OneDrive - Vereniging van Radio Zendamateurs/GitHub/"
              r"PTV_Preservation/PM5644/PM5644G00/ROM_Dumps")

ROMS = {
    "y0": "EPROM_4008_102_56191_CSUM_9A6A.bin",
    "y1": "EPROM_4008_102_56201_CSUM_4E67.bin",
    "y2": "EPROM_4008_102_56211_CSUM_3172.bin",
    "y3": "EPROM_4008_102_56221_CSUM_7F95.bin",
    "lsb": "EPROM_4008_102_56231_CSUM_78C4.bin",
    "ry0": "EPROM_4008_102_56241_CSUM_F0DF.bin",
    "ry1": "EPROM_4008_102_56251_CSUM_F397.bin",
    "by0": "EPROM_4008_102_56261_CSUM_2DA9.bin",
    "by1": "EPROM_4008_102_56271_CSUM_2E0F.bin",
    "cpu": "EPROM_4008_102_59371_CSUM_A100.bin",
}

VT_START, VT_LEN = 0x52F6, 0xD98
FIELD_LINES = 290

# The active line starts this many luma samples after 0H (two chroma
# samples a pair: chroma column = pair + LINE_X0 / 2).
LINE_X0 = 138


def _load(base=EPROM_BASE, roms=ROMS):
    d = {}
    for k, f in roms.items():
        b = open(os.path.join(base, f), "rb").read()
        # The file name carries the 16-bit byte sum of the EPROM.
        want = int(f.split("_CSUM_")[1][:4], 16)
        if sum(b) & 0xFFFF != want:
            raise SystemExit("%s: checksum %04X, file name says %04X" % (f, sum(b) & 0xFFFF, want))
        d[k] = np.frombuffer(b, np.uint8).astype(np.int32)
    lsb = d["lsb"]
    luma = np.stack([(d["y0"] << 2) | ((lsb & 0x10) >> 3) | (lsb & 0x01),
                     (d["y1"] << 2) | ((lsb & 0x20) >> 4) | ((lsb & 0x02) >> 1),
                     (d["y2"] << 2) | ((lsb & 0x40) >> 5) | ((lsb & 0x04) >> 2),
                     (d["y3"] << 2) | ((lsb & 0x80) >> 6) | ((lsb & 0x08) >> 3)], 1).reshape(-1)
    ry = np.stack([d["ry0"], d["ry1"]], 1).reshape(-1)
    by = np.stack([d["by0"], d["by1"]], 1).reshape(-1)
    return luma, ry, by, d["cpu"]


def _address(e, kind, per_unit):
    """Utility.DecodeVector(): kind 0 back porch, 1 centre, 2 front porch."""
    i1, i2, i3 = e
    seq = ((0x00, 0x00, 0x40), (0x00, 0x80, 0x40), (0x80, 0x00, 0xC0), (0x80, 0x80, 0xC0))[i1 & 3]
    a = (i3 if kind == 1 else i2) << 8 | seq[kind]
    if (a & 0x100) == 0 and (i1 & 0x20):
        a |= 0x10000
    if ((a & 0x100) or kind == 1) and (i1 & 0x10):
        a |= 0x10000
    if i1 & 0x04:
        a |= 0x20000
    if i1 & 0x08:
        a |= 0x40000
    return a * per_unit


def fields(pattern):
    """The four fields of vector table `pattern`, as played: a list of
    (luma[290, 864], R-Y[290, 432], B-Y[290, 432]), raw EPROM values."""
    luma, ry, by, cpu = _load()
    t = cpu[VT_START + pattern * VT_LEN:VT_START + (pattern + 1) * VT_LEN]
    out = []
    for f in range(4):
        ys, rs, bs = [], [], []
        for i in range(FIELD_LINES):
            o = 3 * (FIELD_LINES * f + i)
            e = (int(t[o]), int(t[o + 1]), int(t[o + 2]))
            y, r, b = [], [], []
            for kind, n in ((0, 64), (1, 120), (2, 32)):
                a = _address(e, kind, 4)
                y.append(luma[a:a + 4 * n])
                c = _address(e, kind, 2)
                r.append(ry[c:c + 2 * n])
                b.append(by[c:c + 2 * n])
            ys.append(np.concatenate(y))
            rs.append(np.concatenate(r))
            bs.append(np.concatenate(b))
        out.append((np.array(ys), np.array(rs), np.array(bs)))
    return out


# Levels, the conversion the earlier render used, read back from its
# tables (every flat level reproduced): luma to 10-bit BT.601 (black 64,
# white 940) and down to 8 bit by truncation; chroma U and V back to Cb
# and Cr, both inverted in the EPROM.
def luma_bt601(v):
    # Clipped at black: the one sync-level stretch inside the window (the
    # equalising pulse on B1) cannot be carried by BT.656.
    return np.clip((64 + np.round((724 - v) * 876.0 / 560.0).astype(np.int32)) >> 2, 16, 235)


def cb_bt601(v):
    return np.clip(128 - np.round((v - 128) * 1.825).astype(np.int32), 1, 254)


def cr_bt601(v):
    return np.clip(128 - np.round((v - 128) * 1.294).astype(np.int32), 1, 254)


def frame(pattern, phase, rows=576, width=720):
    """BT.656 rows (rows x 2*width bytes, Cb Y Cr Y) of generator frame
    `phase` (0 or 1): field 2 rows from field A of that frame, field 1
    rows from field B. Chroma stays where the EPROM has it, no shift."""
    fa, fb = fields(pattern)[2 * phase:2 * phase + 2]
    lines = np.zeros((rows, width * 2), np.uint8)
    c0 = LINE_X0 // 2
    def put(row, f, i):
        y_, r_, b_ = f
        lines[row, 1::2] = luma_bt601(y_[i, LINE_X0:LINE_X0 + width])
        lines[row, 0::4] = cb_bt601(b_[i, c0:c0 + width // 2])
        lines[row, 2::4] = cr_bt601(r_[i, c0:c0 + width // 2])

    for f, first in ((fa, -1), (fb, -2)):
        for i in range(FIELD_LINES):
            if 0 <= 2 * i + first < rows:
                put(2 * i + first, f, i)
    put(0, fa, 0)            # line 23, the top half line
    put(rows - 1, fb, 1)     # line 623, the bottom half line
    return lines


# --- G924 ------------------------------------------------------------
#
# Source: the REV_1 G924 EPROMs of inaxeon/PTV_Preservation
# (PM5644/PM5644G924/ROM_Dumps/REV_1; REV_2 lacks the B-Y pair). Layout
# from PhilipsPatternRom (RomManager.cs: PAL_16_9, vector table 0x6D1D +
# n x 0x918; Utility.LoadVectors: two bytes a line, entry = (VT[i+1],
# VT[i]); PatternRenderer.DrawLine_WideScreen: centre = high << 8 |
# 0x10000 if control & 0x20 | 0x20000 if & 0x04 | 0x40000 if & 0x08; a
# line is 2048 luma samples from centre x 4 - 1024 and 1024 chroma from
# centre x 2 - 512). 1164 vectors: four fields of 291.
#
# Sampled at 20.0 MHz: the equalising pulse on the half line starts 640
# samples after the line sync, half of a 64 us line. That cannot pass
# BT.656 one for one, so the active window (1045 samples from 218, laid
# against the earlier render: mean 0.21 code) is averaged down to 707
# samples, centred with 6 black samples each side, as before.
#
# Rows: vector i of field A is row 2i + 1, of field B row 2i (same fit).
# A0 is line 23 instead: a data burst from 10.95 us (not WSS; it does not
# fit the EN 300 294 run-in and start code at any rate) and the border in
# the second half; B289 is line 623 (border, then an equalising pulse at
# mid-line).

G924_BASE = (r"c:/Users/pe5pv/OneDrive - Vereniging van Radio Zendamateurs/GitHub/"
             r"PTV_Preservation/PM5644/PM5644G924/ROM_Dumps/REV_1")
G924_ROMS = {
    "y0": "EPROM_4008_002_00551_CSUM_9F93.BIN",
    "y1": "EPROM_4008_002_00561_CSUM_3998.BIN",
    "y2": "EPROM_4008_002_00571_CSUM_5608.BIN",
    "y3": "EPROM_4008_002_00581_CSUM_739C.BIN",
    "lsb": "EPROM_4008_002_00591_CSUM_1C84.BIN",
    "ry0": "EPROM_4008_002_00601_CSUM_F478.BIN",
    "ry1": "EPROM_4008_002_00611_CSUM_F167.BIN",
    "by0": "EPROM_4008_002_00621_CSUM_06E1.BIN",
    "by1": "EPROM_4008_002_00631_CSUM_FFB8.BIN",
    "cpu": "EPROM_4008_002_00541_CSUM_B700.BIN",
}
G924_VT_START, G924_VT_LEN = 0x6D1D, 0x918
G924_FIELD_LINES = 291
G924_X0, G924_W = 218, 1045
G924_ACTIVE = 707


def g924_fields(pattern):
    """The four fields of G924 vector table `pattern`: (luma[291, 2048],
    R-Y[291, 1024], B-Y[291, 1024]), raw EPROM values."""
    luma, ry, by, cpu = _load(G924_BASE, G924_ROMS)
    t = cpu[G924_VT_START + pattern * G924_VT_LEN:G924_VT_START + (pattern + 1) * G924_VT_LEN]
    out = []
    for f in range(4):
        ys, rs, bs = [], [], []
        for i in range(G924_FIELD_LINES):
            o = 2 * (G924_FIELD_LINES * f + i)
            hi, ctl = int(t[o + 1]), int(t[o])
            c = hi << 8
            if ctl & 0x20:
                c |= 0x10000
            if ctl & 0x04:
                c |= 0x20000
            if ctl & 0x08:
                c |= 0x40000
            ys.append(luma[c * 4 - 1024:c * 4 + 1024])
            rs.append(ry[c * 2 - 512:c * 2 + 512])
            bs.append(by[c * 2 - 512:c * 2 + 512])
        out.append((np.array(ys), np.array(rs), np.array(bs)))
    return out


def g924_frame(pattern, phase, levels=64):
    """BT.656 rows of G924 generator frame `phase`, averaged down to 707
    samples the way the earlier render was (convert_rom_pattern.py
    area_resample and reduce_levels), without the chroma shift: the
    chroma stays where the EPROM has it."""
    from convert_rom_pattern import area_resample, reduce_levels
    fa, fb = g924_fields(pattern)[2 * phase:2 * phase + 2]
    rows = 576
    order = []
    for i in range(288):
        order.append((2 * i, fb, i))
        order.append((2 * i + 1, fa, i))
    order[0] = (0, fa, 0)        # line 23
    order[1] = (1, fa, 1)        # the top border, which A0 left
    order[-1] = (rows - 1, fb, 289)   # line 623
    Y = np.zeros((rows, G924_ACTIVE))
    CB = np.zeros((rows, G924_ACTIVE // 2))
    CR = np.zeros((rows, G924_ACTIVE // 2))
    c0, c1 = G924_X0 // 2, (G924_X0 + G924_W) // 2
    for row, (y_, r_, b_), i in order:
        y10 = 64 + np.round((724 - y_[i, G924_X0:G924_X0 + G924_W]) * 876.0 / 560.0)
        Y[row] = area_resample((y10.astype(np.int64) >> 2).astype(np.float64)[None, :], G924_ACTIVE)[0]
        cb = 128 - np.round((b_[i, c0:c1] - 128) * 1.825)
        cr = 128 - np.round((r_[i, c0:c1] - 128) * 1.294)
        CB[row] = area_resample(cb[None, :], G924_ACTIVE // 2)[0]
        CR[row] = area_resample(cr[None, :], G924_ACTIVE // 2)[0]
    Y = np.clip(np.round(reduce_levels(Y, levels)), 16, 235).astype(np.uint8)
    CB = np.clip(np.round(CB), 16, 240).astype(np.uint8)
    CR = np.clip(np.round(CR), 16, 240).astype(np.uint8)
    pad = 6
    lines = np.zeros((rows, 720 * 2), np.uint8)
    lines[:, 0::4] = 128
    lines[:, 2::4] = 128
    lines[:, 1::2] = 16
    for i in range(G924_ACTIVE):
        lines[:, (pad + i) * 2 + 1] = Y[:, i]
    for i in range(CB.shape[1]):
        px = pad + i * 2
        lines[:, px * 2] = CB[:, i]
        lines[:, px * 2 + 2] = CR[:, i]
    return lines

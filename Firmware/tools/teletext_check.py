"""Recomputes the teletext waveform of the firmware.

    python tools/teletext_check.py
    python tools/teletext_check.py --gpp C:/msys64/ucrt64/bin/g++.exe
    python tools/teletext_check.py --plot out.png

Why this exists: the bits of World System Teletext do NOT fall on our
sample grid, 13.5 MHz / 6.9375 Mbit/s is 72/37 = 1.946 samples per bit,
so the line is worked out as a band limited waveform and not switched per
sample. Whether that is right cannot be seen from the code. So
src/teletext.cpp is built ITSELF on the pc here (with -DTT_HOSTTEST), the
waveform is fetched, and it is then demodulated blind as though it were
an unknown signal: find the 50 % crossings, estimate the bit rate and the
phase from those, sample at the bit centres, recover the framing code and
only then read out the bytes. Those bytes have to be equal to what the
firmware said it put in.

Nothing is assumed: the bit rate, the starting moment, the amplitudes and
the bandwidth are all four MEASURED and only then compared against the
standard.

Standard values that are tested against:
  ETS 300 706 (May 1997)
    5.2   0 level = black +/- 2 %; 1 level = 66 +/- 6 % of black to white
    5.3   6.9375 Mbit/s
    5.4   spectrum practically zero at 5 MHz
    6.1   clock run-in 1010101010101010
    6.2   framing code 11100100 (transmission order)
    6.3   centre of the last but one '1' of the run-in at 12.0 us
          (+0.4 / -1.0) after 0H
    7.1   45 bytes = 360 bits, LSB first
    8.1   odd parity; 8.2 Hamming 8/4
  ITU-R BT.601-5, appendix 1 fig. 1: the digital active line starts
    132 sample periods after 0H; black = 16, peak white = 235.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "src", "teletext.cpp")

FS = 13.5e6                 # luma sampling frequency (BT.601)
BITRATE = 6.9375e6          # ETS 5.3
BP_NORM = 72.0 / 37.0       # samples per bit, exact
ACT_START = 132             # BT.601-5 fig. 1: sample 0 lies 132 T after 0H
BLACK, PEAK_WHITE = 16.0, 235.0

# Candidates for a pc compiler. The PlatformIO toolchain cannot be used
# here: that one builds for ARM and not for the pc.
GPP_CANDIDATES = [
    "g++",
    r"C:\msys64\ucrt64\bin\g++.exe",
    r"C:\msys64\mingw64\bin\g++.exe",
    r"C:\ProgramData\chocolatey\bin\g++.exe",
]


def find_gpp(given):
    if given:
        return given
    for c in GPP_CANDIDATES:
        p = shutil.which(c) if os.sep not in c else (c if os.path.exists(c) else None)
        if p:
            return p
    raise SystemExit(
        "no pc compiler found. Give one with --gpp, for example\n"
        "  python tools/teletext_check.py --gpp C:/msys64/ucrt64/bin/g++.exe"
    )


def build_and_run(gpp):
    """Builds src/teletext.cpp for the pc and fetches the output."""
    tmp = tempfile.mkdtemp(prefix="tt_")
    exe = os.path.join(tmp, "tt_host.exe")
    env = dict(os.environ)
    # The msys compilers need their own bin directory for the dlls.
    env["PATH"] = os.path.dirname(gpp) + os.pathsep + env.get("PATH", "")
    cmd = [gpp, "-std=gnu++17", "-O2", "-DTT_HOSTTEST", "-I", os.path.join(ROOT, "src"),
           SOURCE, "-o", exe, "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise SystemExit("build failed:\n" + r.stdout + r.stderr)
    r = subprocess.run([exe], capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise SystemExit("run failed:\n" + r.stdout + r.stderr)
    return r.stdout


def read_output(text):
    """Turns the output of the host test into constants and packets."""
    con, packets = {}, {}
    for line in text.splitlines():
        d = line.split()
        if not d or d[0].startswith("#"):
            continue
        if d[0] == "packet":
            p = int(d[1])
            packets.setdefault(p, {})
            if d[2] == "bytes":
                packets[p]["bytes"] = np.array([int(x, 16) for x in d[3:]], dtype=np.uint8)
            else:
                packets[p]["luma"] = np.array([int(x) for x in d[3:]], dtype=np.float64)
        else:
            for i in range(0, len(d) - 1, 2):
                con[d[i]] = float(d[i + 1])
    return con, packets


# --- the demodulator -------------------------------------------------


def sinc_interp(v, t, halfwidth=24):
    """Samples the band limited signal v at arbitrary times t.

    Allowed: the waveform runs to 5 MHz and Nyquist lies at 6.75 MHz, so
    the samples capture it completely. This is therefore not an
    approximation but the reconstruction itself; the Blackman window only
    cuts the tail off.
    """
    t = np.atleast_1d(np.asarray(t, dtype=np.float64))
    n = np.arange(-halfwidth, halfwidth + 1)
    base = np.floor(t).astype(int)[:, None] + n[None, :]
    dx = t[:, None] - base
    w = np.blackman(2 * halfwidth + 1)[None, :]
    kernel = np.sinc(dx) * w
    safe = np.clip(base, 0, len(v) - 1)
    sample = np.where((base >= 0) & (base < len(v)), v[safe], 0.0)
    return (sample * kernel).sum(1) / (kernel.sum(1))


def crossings(v, threshold=0.5, upto=None):
    """Sub sample accurate places where v passes the threshold.

    First roughly between two samples, and after that 40 rounds of
    halving on the RECONSTRUCTED waveform. That last part is needed:
    interpolating in a straight line between two samples is
    systematically off because the waveform curves there, and that error
    goes straight into the measured bit rate.
    """
    x = v[:upto] - threshold
    idx = np.nonzero(np.sign(x[:-1]) * np.sign(x[1:]) < 0)[0]
    lo = idx.astype(np.float64)
    hi = lo + 1.0
    slo = np.sign(x[idx])
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        sm = np.sign(sinc_interp(v, mid) - threshold)
        take = (sm == slo)
        lo = np.where(take, mid, lo)
        hi = np.where(take, hi, mid)
    return 0.5 * (lo + hi)


def estimate_bit_clock(v):
    """Get the bit period and phase blind out of the clock run-in.

    The run-in is 1010101010101010, so at the start of the line there is
    a series of edges exactly one bit period apart. We take the longest
    unbroken series of crossings with equal spacing and draw a straight
    line through those. Not a single standard value goes in here; the bit
    rate comes out of the measurement.
    """
    k = crossings(v, upto=60)
    if len(k) < 8:
        raise SystemExit("no clock run-in found: %d crossings" % len(k))
    d = np.diff(k)
    med = np.median(d)
    best, current = (0, 0), 0
    for i in range(len(d)):
        if abs(d[i] - med) < 0.05 * med:
            current += 1
            if current > best[1] - best[0]:
                best = (i + 1 - current, i + 1)
        else:
            current = 0
    a, b = best
    if b - a < 8:
        raise SystemExit("clock run-in too short: %d equal steps" % (b - a))
    m = np.arange(a, b + 1)
    bp, t0 = np.polyfit(m, k[a:b + 1], 1)
    residual = k[a:b + 1] - (bp * m + t0)
    return bp, bp * a + t0, float(np.abs(residual).max()), b - a + 1


FRAMING_BITS = [1, 1, 1, 0, 0, 1, 0, 0]  # ETS 6.2, transmission order


def demodulate(luma):
    """From 720 luma bytes back to 45 bytes, without prior knowledge."""
    v = (luma - BLACK) / (PEAK_WHITE - BLACK)  # 0 = black, 1 = peak white
    one = 0.66                                 # expected 1 level (ETS 5.2)
    s = v / one                                # normalised to 0..1
    bp, t_first, residual, count = estimate_bit_clock(s)

    # Bit centres: half a bit period after every edge position, and then
    # on to the end of the active window.
    n_max = int((len(s) - 2 - (t_first + 0.5 * bp)) / bp)
    tc = t_first + 0.5 * bp + bp * np.arange(n_max)
    level = sinc_interp(s, tc)
    bits = (level > 0.5).astype(int)

    # Look for the framing code, which is what a decoder does too.
    target = np.array(FRAMING_BITS)
    pos = -1
    for i in range(0, min(len(bits) - 8, 40)):
        if np.array_equal(bits[i:i + 8], target):
            pos = i
            break
    if pos < 0:
        raise SystemExit("framing code 11100100 not recovered")

    # The framing code is byte 3, so bits 16 through 23 of the packet.
    # Now that the numbering is fixed, the 360 bit centres of the packet
    # itself can be worked out, including the first ones, which may lie
    # before the first edge that was found.
    t_bit0 = t_first + (0.5 + pos - 16) * bp
    tc = t_bit0 + bp * np.arange(360)
    if tc[-1] > len(s) - 2:
        raise SystemExit("the last bit falls outside the active window "
                         "(sample %.1f of %d)" % (tc[-1], len(s)))
    b = (sinc_interp(s, tc) > 0.5).astype(int)
    if not np.array_equal(b[16:24], target):
        raise SystemExit("framing code not in its place after aligning")

    # SECOND PASS. The run-in is only 16 bits long and at both ends the
    # symmetry is not there: before bit 0 there is nothing and after bit
    # 15 comes the framing code. The estimate from those 15 edges alone
    # is therefore a few hundred ppm out, and over 360 bits that builds up
    # to a third of a sample. Now that the bits are known it can be done
    # over the WHOLE line: every place where two consecutive bits differ
    # ought to have a 50 % crossing, and a straight line goes through all
    # those crossings. That is also what a real decoder does: its PLL runs
    # the whole line through and not just on the run-in.
    m = np.nonzero(b[1:] != b[:-1])[0] + 1
    expected = t_bit0 + (m - 0.5) * bp
    measured = []
    good = []
    for i, tv in zip(m, expected):
        lo, hi = tv - 0.6 * bp, tv + 0.6 * bp
        slo = np.sign(sinc_interp(s, lo)[0] - 0.5)
        shi = np.sign(sinc_interp(s, hi)[0] - 0.5)
        if slo == shi:
            continue          # no clean single crossing, skip it
        for _ in range(40):
            mid = 0.5 * (lo + hi)
            if np.sign(sinc_interp(s, mid)[0] - 0.5) == slo:
                lo = mid
            else:
                hi = mid
        measured.append(0.5 * (lo + hi))
        good.append(i)
    good = np.array(good, dtype=np.float64)
    measured = np.array(measured)
    bp2, t02 = np.polyfit(good - 0.5, measured, 1)
    jitter = measured - (bp2 * (good - 0.5) + t02)

    tc = t02 + bp2 * np.arange(360)
    level = sinc_interp(s, tc)
    b = (level > 0.5).astype(int)
    if not np.array_equal(b[16:24], target):
        raise SystemExit("framing code gone after the second pass")
    bytes_ = np.packbits(b.reshape(45, 8), axis=1, bitorder="little").ravel()

    return dict(bp=bp2, residual=residual, runin=count, bits=b, bytes=bytes_,
                level=level, tc=tc, v=v, s=s,
                edges=len(good), jitter=float(np.abs(jitter).max()),
                jitter_rms=float(np.sqrt((jitter ** 2).mean())))


# --- teletext decoding -----------------------------------------------


def hamming84_decode(byte):
    """ETS 8.2. Gives the nibble, or None if it does not check out."""
    b = [(byte >> i) & 1 for i in range(8)]  # b[0] = bit 1 = LSB
    p1, d1, p2, d2, p3, d3, p4, d4 = b
    a = 1 ^ p1 ^ d1 ^ d3 ^ d4
    bb = 1 ^ p2 ^ d1 ^ d2 ^ d4
    c = 1 ^ p3 ^ d1 ^ d2 ^ d3
    dd = 1 ^ p1 ^ d1 ^ p2 ^ d2 ^ p3 ^ d3 ^ p4 ^ d4
    if a or bb or c:
        return None if not dd else None
    return d1 | (d2 << 1) | (d3 << 2) | (d4 << 3)


def parity_ok(byte):
    return bin(byte).count("1") % 2 == 1  # ETS 8.1: odd


def readable(byte):
    c = byte & 0x7F
    if c < 0x20:
        return "<%d>" % c
    return chr(c)


# --- main program -----------------------------------------------------


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gpp", help="path to a pc g++")
    ap.add_argument("--plot", help="write the waveform of packet 0 out as a png")
    a = ap.parse_args()

    gpp = find_gpp(a.gpp)
    print("compiler: %s" % gpp)
    con, packets = read_output(build_and_run(gpp))
    print("firmware reports: alpha=%.6f  taps=%d  phases=%d  packets=%d"
          % (con["alpha"], con["taps"], con["phases"], con["packets"]))

    faults = []
    all_bp, all_residual = [], []
    zero_levels, one_levels = [], []
    quiet_zero_l, quiet_one_l = [], []
    minluma, maxluma = 255, 0
    first = None

    for p in sorted(packets):
        expected = packets[p]["bytes"]
        luma = packets[p]["luma"]
        minluma = min(minluma, int(luma.min()))
        maxluma = max(maxluma, int(luma.max()))
        r = demodulate(luma)
        if first is None:
            first = (p, r, luma)
        all_bp.append(r["bp"])
        all_residual.append(r["residual"])
        zero_levels.append(r["level"][r["bits"] == 0])
        one_levels.append(r["level"][r["bits"] == 1])
        # quiet = three equal neighbours on either side
        b = r["bits"]
        same = np.ones(len(b), dtype=bool)
        for k in range(1, 4):
            same[3:-3] &= (b[3 - k:len(b) - 3 - k] == b[3:-3])
            same[3:-3] &= (b[3 + k:len(b) - 3 + k] == b[3:-3])
        same[:3] = same[-3:] = False
        quiet_zero_l.append(r["level"][same & (b == 0)])
        quiet_one_l.append(r["level"][same & (b == 1)])
        if not np.array_equal(r["bytes"], expected):
            bad = np.nonzero(r["bytes"] != expected)[0]
            faults.append("packet %d: %d byte(s) wrong, the first at index %d "
                          "(%02X became %02X)"
                          % (p, len(bad), bad[0], expected[bad[0]],
                             r["bytes"][bad[0]]))

    print()
    print("=== 1. bytes there and back ===")
    print("packets recomputed: %d, each 45 bytes = %d bytes"
          % (len(packets), 45 * len(packets)))
    if faults:
        for f in faults:
            print("  FAULT " + f)
    else:
        print("  ALL bytes come out again exactly as they went in.")

    print()
    print("=== 2. bit rate and time reference ===")
    bp = float(np.mean(all_bp))
    print("measured bit period : %.6f sample  (standard 72/37 = %.6f)" % (bp, BP_NORM))
    print("deviation           : %+.1f ppm" % ((bp / BP_NORM - 1) * 1e6))
    print("used: %d edges over the whole line (%d in the run-in for the "
          "first estimate)" % (first[1]["edges"], first[1]["runin"]))
    print("edge jitter against the straight line: %.4f sample rms, %.4f "
          "sample worst case" % (first[1]["jitter_rms"], first[1]["jitter"]))
    print("                                      = %.1f ps rms, on a "
          "bit period of %.1f ns"
          % (first[1]["jitter_rms"] / FS * 1e12, 1e9 / BITRATE))

    # ETS 6.3: the centre of bit 13 (1 based), so index 12, lies 12.0 us
    # (+0.4 / -1.0) after 0H. Sample 0 lies 132 T after 0H (BT.601-5).
    t13 = first[1]["tc"][12]
    us = (ACT_START + t13) / FS * 1e6
    print("centre of bit 13    : sample %.4f -> %.4f us after 0H (standard 12.0 "
          "+0.4/-1.0)" % (t13, us))
    if not (11.0 <= us <= 12.4):
        faults.append("time reference %.3f us falls outside 11.0..12.4" % us)

    print()
    print("=== 3. amplitudes (ETS 5.2) ===")
    # ETS 5.2 fixes the two LEVELS of the NRZ signal, not the momentary
    # value at every bit centre of the shaped waveform. A shaped pulse
    # rings, that is simply how it is. So it is measured twice: the QUIET
    # LEVELS (bit centres with three equal bits on either side, where the
    # waveform has settled) against the standard, and alongside that the
    # whole eye as a measure of quality.
    quiet_zero = np.concatenate(quiet_zero_l)
    quiet_one = np.concatenate(quiet_one_l)
    zero = np.concatenate(zero_levels)
    one = np.concatenate(one_levels)
    for name, v, target, tol in (("0 level", quiet_zero * 66.0, 0.0, 2.0),
                                 ("1 level", quiet_one * 66.0, 66.0, 6.0)):
        print("%s at rest (%5d bit centres): %7.3f .. %7.3f %% of black to white  "
              "(standard %.0f +/- %.0f %%)" % (name, len(v), v.min(), v.max(), target, tol))
        if v.min() < target - tol or v.max() > target + tol:
            faults.append("%s %.2f..%.2f %% outside the standard" % (name, v.min(), v.max()))
    print("all bit centres (so including ringing): 0 level %+.3f..%+.3f %%, "
          "1 level %.3f..%.3f %%"
          % (zero.min() * 66, zero.max() * 66, one.min() * 66, one.max() * 66))
    margin = min(one.min() - 0.5, 0.5 - zero.max())
    print("eye opening: the smallest distance from a bit centre to the "
          "decision threshold is %.4f (%.1f %% of the amplitude)" % (margin, margin * 100))
    if margin <= 0:
        faults.append("the eye is closed: a bit centre lies on the wrong side")

    print()
    print("=== 4. code values and bandwidth ===")
    print("luma stays between %d and %d (black 16, 1 level 161, peak white 235)"
          % (minluma, maxluma))
    print("clipped at code 1: %d of %d samples (%.2f %%); the deepest "
          "undershoot wanted to go to code %d, which is %.1f %% of the data "
          "amplitude below black (there is only 10.3 %% of room)"
          % (con["clip"], 720 * con["packets"], 100 * con["clip"] / (720 * con["packets"]),
             con["clipdeepest"], (16 - con["clipdeepest"]) / 145 * 100))
    print("0x00 and 0xFF are reserved for EAV/SAV in BT.656: %s"
          % ("good, they do not occur" if minluma >= 1 and maxluma <= 254 else "FAULT"))
    if minluma < 1 or maxluma > 254:
        faults.append("luma touches 0 or 255")

    v = first[2] - first[2].mean()   # take the dc off, otherwise dc is the peak
    spec = np.abs(np.fft.rfft(v * np.hanning(len(v))))
    f = np.fft.rfftfreq(len(v), 1 / FS)
    tot = (spec ** 2).sum()
    above5 = (spec[f > 5.0e6] ** 2).sum()
    above675 = (spec[f > FS / 2 * 0.999] ** 2).sum()
    print("energy above 5.0 MHz : %.4f %% of the total (ETS 5.4: practically zero)"
          % (100 * above5 / tot))
    print("energy above Nyquist : %.6f %%, has to be zero, otherwise there "
          "was aliasing in it" % (100 * above675 / tot))
    # The clock run-in is a pure alternation 1010..., so there has to be a
    # sine at HALF the bit rate there. That can be measured separately by
    # transforming only that piece of the line.
    ri = first[2][2:36].astype(float)
    ri = ri - ri.mean()
    sri = np.abs(np.fft.rfft(ri * np.hanning(len(ri))))
    fri = np.fft.rfftfreq(len(ri), 1 / FS)
    # centre of gravity around the peak, otherwise the bin spacing
    # (400 kHz) is the largest source of error
    i = int(np.argmax(sri))
    w = sri[i - 1:i + 2] ** 2
    top = (fri[i - 1:i + 2] * w).sum() / w.sum()
    print("clock run-in          : %.3f MHz measured (has to be half the "
          "bit rate, %.3f MHz)" % (top / 1e6, BITRATE / 2e6))
    if abs(top - BITRATE / 2) > 0.15e6:
        faults.append("the run-in lies at %.3f MHz instead of %.3f MHz"
                      % (top / 1e6, BITRATE / 2e6))
    if above5 / tot > 0.001:
        faults.append("more than 0.1 %% of the energy lies above 5 MHz")

    print()
    print("=== 5. what the page says ===")
    for p in sorted(packets):
        b = packets[p]["bytes"]
        mag_nib = hamming84_decode(b[3])
        row_nib = hamming84_decode(b[4])
        if mag_nib is None or row_nib is None:
            faults.append("packet %d: Hamming 8/4 in byte 4/5 does not check out" % p)
            continue
        mag = mag_nib & 7
        row = ((mag_nib >> 3) & 1) | (row_nib << 1)
        if row != p:
            faults.append("packet %d reports itself as row %d" % (p, row))
        if p == 0:
            units = hamming84_decode(b[5])
            tens = hamming84_decode(b[6])
            c11 = (hamming84_decode(b[12]) or 0) & 1
            text = "".join(readable(x) for x in b[13:45])
            bad = sum(0 if parity_ok(x) else 1 for x in b[13:45])
            print("  X/0  magazine %d, page %X%X%X, C11(serial)=%d" %
                  (mag, mag, tens, units, c11))
            print("       header: |%s|" % text)
        else:
            text = "".join(readable(x) for x in b[5:45])
            bad = sum(0 if parity_ok(x) else 1 for x in b[5:45])
            print("  X/%-2d |%s|" % (row, text))
        if bad:
            faults.append("packet %d: %d text bytes without odd parity" % (p, bad))
    print("  (<n> is a spacing attribute: 1..7 are the alpha colours, ETS 12.2)")

    if a.plot:
        make_plot(a.plot, first)

    print()
    if faults:
        print("=== NOT IN ORDER ===")
        for f in faults:
            print("  " + f)
        sys.exit(1)
    print("=== ALL IN ORDER ===")
    print("The waveform has been demodulated blind and yields exactly the same 45")
    print("bytes per packet as the firmware put into it, with the bit rate, the")
    print("time reference, the amplitudes and the bandwidth within the standard.")


def make_plot(path, first):
    try:
        import matplotlib
    except ImportError:
        print("no plot: matplotlib is missing (pip install matplotlib)")
        return
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    p, r, luma = first
    fig, ax = plt.subplots(2, 1, figsize=(12, 7))
    n = 80
    t = np.arange(n)
    fine = np.linspace(0, n - 1, 2000)
    ax[0].plot(fine, sinc_interp(luma, fine), "-", lw=1, label="band limited waveform")
    ax[0].plot(t, luma[:n], "o", ms=3, label="the 720 luma samples")
    tc = r["tc"][:40]
    ax[0].plot(tc, sinc_interp(luma, tc), "rx", ms=6, label="measured bit centres")
    ax[0].axhline(16, color="k", lw=0.5)
    ax[0].axhline(161, color="k", lw=0.5)
    ax[0].set_title("packet %d: clock run-in, framing code and the start of the address" % p)
    ax[0].set_xlabel("luma sample (13.5 MHz)")
    ax[0].set_ylabel("luma code")
    ax[0].legend(fontsize=8)
    spec = np.abs(np.fft.rfft((luma - 16) * np.hanning(len(luma))))
    f = np.fft.rfftfreq(len(luma), 1 / FS) / 1e6
    ax[1].semilogy(f, spec / spec.max())
    ax[1].axvline(BITRATE / 2e6, color="g", ls="--", label="half the bit rate 3.469 MHz")
    ax[1].axvline(5.0, color="r", ls="--", label="5 MHz (ETS 5.4)")
    ax[1].set_xlabel("MHz")
    ax[1].set_ylabel("relative")
    ax[1].legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    print("plot written to %s" % path)


if __name__ == "__main__":
    main()

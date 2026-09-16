"""
Computes the insertion test signals (ITS, also VITS) for 625/50 as 720
samples in 4:2:2, ready to be put into the field blanking by the
firmware.

    python tools/make_its.py --png its.png

Delivers 1440 bytes per ITS line (720 pixels Cb,Y,Cr,Y), plus a PNG on
which the four lines are drawn the way the standard pictures them, so
that you can lay them next to Figure 1 to 4 of the standard.

THE SOURCE. Everything below comes from ITU-T Rec. J.63 (06/90),
"Insertion of test signals in the field-blanking interval of monochrome
and colour television signals", formerly Recommendation ITU-R CMTT.473,
that is the document called "BT.473" in the assignment. Annex I is about
625 line systems. Where "J.63 par. x.y" is written below, it says so
there literally. The four waveforms are in Figure 1 (line 17), 2 (line
18), 3 (line 330) and 4 (line 331).

WHICH LINES. J.63 Annex I par. 1: "For the international transmission of
625-line television signals, Recommendation 472 and Report 314 propose the
use of lines 17 (330) and 18 (331) for insertion test signals." So:

  line  17  B2 bar, B1 2T pulse, F 20T composite pulse, D1 staircase
  line  18  luminance pedestal, C1 reference bar, C2 multiburst
  line 330  B2 bar, B1 2T pulse, D2 (= staircase D1 with subcarrier on it)
  line 331  luminance pedestal, G1 chroma bar (or G2), E reference carrier

Line 17/18 sit in field 1, line 330/331 in the corresponding place in
field 2. J.63 par. 6, Table II, says what you measure with them: B2 gain
and line time distortion, B1 pulse response, C1+C2 amplitude/frequency
response, F chroma to luma difference in gain and delay, D1 luminance
non-linearity, D2 differential gain, D2+E differential phase, G1/G2
chroma non-linearity.

THE TIME GRID. J.63 Annex I par. 1: "it is assumed that the line duration
H is divided into 32 equal time periods. This division defines the
characteristic instants" and "the characteristic instants are referred to
the mid-amplitude point of the leading edge of the synchronizing pulse".
That last point is called 0H. H = 64 us (ITU-R BT.470-6, Table 1-1, symbol
H, column B/B1/G/H/I/D/D1/K/K1/L), so H/32 = 2 us exactly.

WHERE OUR WINDOW OF 720 SAMPLES LIES: the whole derivation is further
down at WINDOW_OFFSET. In short: 0H falls 132 luma samples before the
first active sample, so sample n belongs to the instant (132 + n) / 13.5
us after 0H, and the characteristic instant k*H/32 falls on sample
27k - 132. Everything in the standard happens between 6H/32 and 31H/32,
that is sample 30 up to and including 705, and so it fits in the 720 with
margin.

WE DO NOT SEND COMPOSITE. This board delivers YCbCr; the ADV7391 makes
the colour subcarrier itself. In composite every chroma element from the
standard is a packet of 4.43 MHz with a certain peak to peak amplitude;
here only its ENVELOPE is given, as a Cb/Cr pair. What that means is at
to_cbcr() and, for the 20T pulse, at build_line17().

WHAT THE FIRMWARE STILL HAS TO DO. Line 17, 18, 330 and 331 fall in the
FIELD BLANKING. BT.656-5 Table 1 puts the V bit of SAV/EAV at 1 for the
lines 624..22 and 311..335, and src/video.cpp does exactly the same; the
active picture is 23..310 and 336..623. So all four of our lines lie
outside the active picture and are filled with blanking for now.

From the datasheet of the ADV7391 (looked up, with page references):

  - Subaddress 0x83 bit 4, "SD Vertical Blanking Interval (VBI) Open"
    (Table 23, p. 34). P. 47: "The ADV739x is able to accept input
    data that contains vertical blanking interval (VBI) data (such as
    CGMS, WSS, VITS)"; if it is off, then "the entire VBI is blanked".
    The reset value of 0x83 is 0x04, so bit 4 = 0: CLOSED by default.
  - P. 47: "VBI data can be present on Line 10 to Line 20 for NTSC and
    on Line 7 to Line 22 for PAL." Line 17 and 18 fall within that.
  - P. 47 on the slave mode this board uses: "if VBI is enabled, the
    blanking bit in the EAV/SAV code is overwritten". The encoder then
    uses its own line counter and not our V bit.
  - OPEN POINT: about line 330 and 331 the datasheet says nothing. For
    PAL it names only "line 7 to 22" and nowhere does it say how the
    second field is numbered. That has to be measured.
  - 0x82 bit 3 "SD Pedestal" is ON after reset (0x82 = 0x0B); for PAL
    it has to be off, otherwise there is a pedestal in these lines.
  - 0x82 bit 7 "SD Active Video Edge Control" multiplies the first and
    the last three samples of the active line by 1/8, 1/2 and 7/8
    (p. 59). That does NOT affect our ITS: the earliest sample we use
    lies at sample 27.7 and the last at 707.6.
  - 0x84 bit 3 "SD Active Video Length" has to stay 0 (720 samples);
    at 1 it becomes 702 for PAL (Table 24, p. 35).
  - The multiburst goes up to 5.8 MHz and only the SSAF luma filter
    reaches that (0x80 bits[4:2] = 100): -3 dB at 6.45 MHz (Table 39,
    p. 49). The ordinary PAL luma filter is at -3 dB at 4.81 MHz and the
    PAL notch filter has a deep hole at exactly 4.4 MHz. The chroma
    envelope fits easily in every chroma filter; the widest is 3.0 MHz
    (0x80 bits[7:5] = 111).
  - OPEN POINT: the datasheet gives the colour coding matrix NOWHERE.
    The numbers 0.493 and 0.877 do not appear in it and there is no
    statement of colour subcarrier amplitude for a given Cb/Cr. The
    conversion in to_cbcr() therefore rests on BT.470-6 and BT.601-7,
    not on the encoder. That the encoder holds to those is plausible but
    not looked up, and has to be measured with a vectorscope, just like
    the requirement that the colour subcarrier lies at 60 degrees from
    the +(B-Y) axis. There is a phase register (0x90 plus 0x9C
    bits[7:6]) but without a statement of degrees per step and without a
    reference axis.

So what is still needed: a hook in src/video.cpp that fills these four
lines instead of writing blanking, and the encoder settings above.

MIND A CLASH. Teletext in PAL uses lines in the same area (0x83 bit 4
opens the VBI in one go for line 7 to 22), and a line can carry only one
of the two. Whoever wants both teletext and ITS has to divide the lines:
keep 17, 18, 330 and 331 free for the ITS and lay the teletext page over
the remaining lines.
"""

import argparse
import math
import os
import sys

import numpy as np

try:
    from PIL import Image, ImageDraw
except ImportError:
    Image = None

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "its_out")

# ---------------------------------------------------------------------
# TIME GRID AND THE WINDOW OF 720 SAMPLES
# ---------------------------------------------------------------------

# Nominal line duration, ITU-R BT.470-6 Table 1-1 symbol H, column
# B,B1,G,H,I,D,D1,K,K1,L: 64 us. J.63 divides that into 32 parts.
H_US = 64.0
H32_US = H_US / 32.0  # 2 us

# Luma sampling, ITU-R BT.601-7: 13.5 MHz, 720 active samples per line.
FS_LUMA_MHZ = 13.5
N_ACTIVE = 720

# 0H LIES 132 LUMA SAMPLES BEFORE THE FIRST ACTIVE SAMPLE.
#
# Derivation from ITU-R BT.656-5, Figure 1 "Composition of interface data
# stream". That figure numbers the luma samples of a line and puts three
# captions with them: "Last sample of digital active line" at 719, "Sample
# data for OH instant" at sample 736, in brackets 732, and note 1 says
# that the numbers in brackets hold for 625 line systems, and "First
# sample of digital active line" at 0. The last blanking sample is 863
# (in brackets; 857 for 525).
#
# For 625 lines 0H therefore falls on luma sample 732, and the line
# counts 864 samples (0..863). From 0H to the next sample 0 that is
# 864 - 732 = 132 samples. That is confirmed by src/video.cpp: it writes
# 1728 bytes per line = SAV (4) + 1440 active + EAV (4) + 280 blanking,
# and 1728 bytes at 27 MHz are 864 luma sample times at 13.5 MHz.
#
# Check against the analogue standard (BT.470-6 Table 1-1): 132/13.5 =
# 9.778 us after 0H the digital active part begins, while the line
# blanking only ends at 10.5 us (symbol b) and the front porch begins 1.5
# us before the next 0H (symbol c, 62.5 us). The digital window is
# therefore 53.333 us against 52 us analogue active, and sticks out by
# 0.72 us in front and 0.61 us behind. That agrees with
# 720 - 52*13.5 = 720 - 702 = 18 samples.
WINDOW_OFFSET = 132


def t_of_sample(n):
    """Instant in us after 0H of luma sample n (n = 0..719)."""
    return (WINDOW_OFFSET + n) / FS_LUMA_MHZ


def instant(k):
    """Instant in us after 0H of the characteristic instant k*H/32."""
    return k * H32_US


def sample_of_instant(k):
    """Luma sample index of instant k*H/32. Exactly 27k - 132."""
    return instant(k) * FS_LUMA_MHZ - WINDOW_OFFSET


# ---------------------------------------------------------------------
# LEVELS
# ---------------------------------------------------------------------

# Amplitude of the luminance bar B2, J.63 par. 2.1: 0.700 +/- 0.007 V,
# measured from blanking level. All other levels in the standard are
# expressed as a fraction of this, so this is the unit of this file.
BAR_V = 0.700

# System B/G/H/I has no setup: black coincides with blanking (BT.470-6
# Table 2 gives no black to blanking difference for these systems,
# unlike M). Blanking = 0 V in the scale below = luma 16.

# BT.601-7 par. 2.5.3: Y = int(219*E'Y + 16), Cb/Cr = int(224*E'C + 128).
Y_BLACK, Y_SPAN = 16.0, 219.0
C_ZERO, C_SPAN = 128.0, 224.0

# BT.601-7 par. 2.5.2: E'Cb = (E'B - E'Y)/1.772 and E'Cr = (E'R - E'Y)/1.402.
KB_601, KR_601 = 1.772, 1.402

# PAL colour coding, ITU-R BT.470-6 Table 2:
#   item 2.5   E'U = 0.493 (E'B - E'Y),  E'V = 0.877 (E'R - E'Y)
#   item 2.9   E'M = E'Y + E'U sin(2*pi*fsc*t) +/- E'V cos(2*pi*fsc*t)
#   item 2.13  amplitude of the colour subcarrier G = sqrt(E'U^2 + E'V^2),
#              and note 14: the unit is the luminance range between
#              blanking and peak white, so our 0.700 V.
KU_PAL, KV_PAL = 0.493, 0.877

# J.63 Annex I par. 1: "in the case of PAL transmissions, the chrominance
# sub-carrier of the insertion signals is locked at 60 +/- 5 degrees from
# the positive (B-Y) axis". The +(B-Y) axis is the +U axis (BT.470-6 item
# 2.9), so the vector lies at 60 degrees from +U.
CHROMA_PHASE_DEG = 60.0

# ---------------------------------------------------------------------
# SHAPING OF THE EDGES AND THE PULSES
# ---------------------------------------------------------------------

# T is the Nyquist interval 1/(2*fc). For PAL fc is put at 5 MHz, so
# T = 100 ns (Tektronix, "PAL Systems Television Measurements",
# 25W-7075-3, Appendix B "Sine-Squared Pulses"). That agrees with J.63
# itself: there the 2T pulse has a half amplitude duration of 200 ns
# (par. 2.2) and the 20T pulse 2 us (par. 2.3), exactly 2x and 20x 100 ns.
T_NS = 100.0

# Half amplitude duration (HAD) of the 2T pulse, J.63 par. 2.2:
# 200 +/- 10 ns.
HAD_2T_US = 0.200

# Half amplitude duration of the 20T pulse, J.63 par. 2.3: 2 +/- 0.06 us.
HAD_20T_US = 2.000

# Shaping of the luminance edges of B2, the pedestal and C1: J.63 par.
# 2.1, 3.1, 3.2 and 5.1 all four say "rise and fall times of transitions:
# derived from the shaping network of the sine-squared pulse (element
# B1)". B1 is the 2T pulse, so those edges are 2T steps: the integral of
# a sin^2 pulse with HAD = 200 ns.
HAD_EDGE_US = HAD_2T_US

# Shaping of the staircase edges D1, J.63 par. 2.4 / 4.3.1: "shaped by a
# Thomson filter (or similar network) with a transfer function modulus
# having its first zero at 4.43 MHz".
#
# INTERPRETATION, and this is the only place in this file where there is
# one. The standard names a filter, not a waveform. A sin^2 shaping with
# half amplitude duration HAD has its first spectral zero at 1/HAD (that
# is the first zero of the Hann window of length 2*HAD; Tektronix
# Appendix B: "sine-squared pulses possess negligible energy at
# frequencies above f = 1/HAD"). A first zero at the colour subcarrier
# frequency therefore gives HAD = 1/4.43361875 MHz = 225.5 ns. With that
# the shaping of the staircase edges is of the same kind as that of all
# the other edges and it meets the only hard criterion the standard names.
FSC_MHZ = 4.43361875  # BT.470-6 Table 2 item 2.11, B/G/H/I: 4 433 618.75 Hz
HAD_STAIRCASE_US = 1.0 / FSC_MHZ

# Shaping of the chroma envelope: J.63 par. 4.3.2, 5.2, 5.3 and 5.4 all
# say "rise and fall times of the envelope of the chrominance signal
# transitions: 1 us approximately". Rise time is the 10-90% time. A sin^2
# step with half amplitude duration HAD has a 10-90% rise time of
# 0.964*HAD (Tektronix Appendix B; computed numerically below in
# check_building_blocks() as well). For 1 us that gives HAD = 1/0.964 us.
RISE_TIME_CHROMA_US = 1.0
STEP_RISE_FACTOR = 0.964
HAD_CHROMA_US = RISE_TIME_CHROMA_US / STEP_RISE_FACTOR


# THE STAIRCASE AND THE 8 BIT GRID, a choice that deserves an explanation.
#
# J.63 par. 2.4 sets two requirements on the staircase D1 that cannot
# both be met on an 8 bit grid:
#   (a) peak to peak within 1 percent of the bar (0.700 V), and
#   (b) largest and smallest step less than 0.5 percent apart.
# The bar is 219 luma codes and 219/5 = 43.8 is not a whole number, so
# with steps that are exactly 1/5 of the bar the codes become 60, 104,
# 147, 191, 235 and those are steps of 44, 44, 43, 44, 44, a difference
# of 2.27 percent, well outside (b). Even with 10 bits 0.57 percent
# remains (876/5 = 175.2), so this is not a matter of more bits.
#
# What CAN be done: steps of exactly 44 codes. Then the staircase is 220
# codes = 0.7032 V = +0.46 percent, still within the 1 percent of (a),
# and the steps are exactly equal, so (b) is met with zero difference.
# The top does end up at code 236 then, a code above peak white, which is
# allowed: BT.601-7 says expressly that the signal "may occasionally
# excurse beyond level 235.00d", but the top of the staircase is then no
# longer exactly equal to the top of the bar.
#
# The equal step is the default here, because that is what D1 is for: you
# measure luminance non-linearity with it by comparing the steps with
# each other, and a source that is itself already 2.27 percent off makes
# that measurement worthless. With --staircase-exact you get the other
# choice.
STAIRCASE_CODES_EQUAL = 220   # 5 x 44
STAIRCASE_CODES_EXACT = 219   # exactly the bar, steps 44/44/43/44/44


def sin2_pulse(t, t_top, had):
    """sin^2 pulse: peak 1 at t_top, half amplitude duration had, zero outside +/- had.

    Tektronix Appendix B: the pulse is the square of half a sine period,
    and is characterised by the half amplitude duration HAD. Written with
    the cosine that is 0.5*(1 + cos(pi*x/had)): value 1 at x=0, value 0.5
    at x = +/- had/2 (so the width at half height = had) and value 0 with
    a tangent touching at x = +/- had.
    """
    x = (t - t_top) / had
    return np.where(np.abs(x) <= 1.0, 0.5 * (1.0 + np.cos(np.pi * np.clip(x, -1, 1))), 0.0)


def sin2_step(t, t_50, had):
    """Step from 0 to 1, 50 percent at t_50, shaped as the integral of sin2_pulse.

    That is what "derived from the shaping network of the sine-squared
    pulse" means: the same shaping, but applied to a step instead of to
    an impulse. The whole transition lasts 2*had; the 10-90% rise time is
    0.964*had.
    """
    u = np.clip((t - (t_50 - had)) / (2.0 * had), 0.0, 1.0)
    return u - np.sin(2.0 * np.pi * u) / (2.0 * np.pi)


# ---------------------------------------------------------------------
# THE ELEMENTS PER LINE
# ---------------------------------------------------------------------
#
# A line is a list of instructions. They are ADDED UP, not laid over each
# other. That is on purpose: an edge is then simply a step with its own
# shaping, and two edges that touch each other (as with G2, where the
# sections are only 2H/32 long while a chroma transition lasts 2.07 us)
# run into each other neatly instead of cutting each other off.
#
#   ("step",  k, delta_volt, had)     level change at instant k*H/32
#   ("pulse", k, amplitude, had)      sin^2 pulse with its peak at k*H/32
#   ("burst", k, f_MHz, amplitude)    sine burst from k*H/32
#
# "step" and "pulse" hold for luma or for the chroma envelope, depending
# on which list they are in.


def staircase_step(exact):
    """Step height of D1 in volts. See the block STAIRCASE AND THE 8 BIT GRID."""
    codes = STAIRCASE_CODES_EXACT if exact else STAIRCASE_CODES_EQUAL
    return (codes / 5.0) * BAR_V / Y_SPAN


def build_line17(mono=False, chroma_on_staircase=False, staircase_exact=False):
    """Line 17, J.63 par. 2 and Figure 1."""
    luma = [
        # 2.1 Luminance bar B2 (reference white): transitions at 6H/32
        # and 11H/32, duration 5H/32, amplitude 0.700 V.
        ("step", 6, BAR_V, HAD_EDGE_US),
        ("step", 11, -BAR_V, HAD_EDGE_US),
        # 2.2 2T sin^2 pulse B1: peak at 13H/32, amplitude equal to the
        # bar (0.700 V), half amplitude duration 200 ns.
        ("pulse", 13, BAR_V, HAD_2T_US),
    ]
    chroma = []
    if not mono:
        # 2.3 Composite 20T pulse F: peak at 16H/32, base at 15H/32 and
        # 17H/32, amplitude equal to the bar (0.700 V), half amplitude
        # duration 2 +/- 0.06 us.
        #
        # WHAT THE PULSE CONSISTS OF. The standard names only the outer
        # dimensions, but it does fix them and the composition follows
        # from that. Tektronix (25W-7075-3, "Chrominance-to-Luminance
        # Gain and Delay"): "This pulse is made up of a sine-squared
        # luminance pulse and a chrominance packet with a sine-squared
        # envelope." Call the luma amplitude L and the peak to peak
        # amplitude of the chroma packet C, both with the same sin^2
        # shape p(t). Then the top of the composite is (L + C/2)*p(t) and
        # the bottom (L - C/2)*p(t). J.63 par. 2.3 requires the bottom to
        # be flat ("perturbations of the pulse base-line ... <= 0,5% peak
        # amplitude") and the peak to be 0.700 V. A flat base means
        # L = C/2; together with L + C/2 = 0.700 that gives L = 0.350 V
        # and C = 0.700 V p-p.
        #
        # THAT THE BASE IS NULL IS EXACTLY WHAT THE PULSE MEASURES, and
        # that is a problem here which this tool can do nothing about: we
        # hand over luma and chroma envelope separately and the encoder
        # does the rest. If the chroma path of the encoder differs in
        # delay or gain from the luma path, and it does, they are
        # different filters, then the base of this pulse is already bent
        # at the output before anything has even been transmitted. The
        # pulse then measures the encoder as well. Computed in composite
        # that would not happen, but composite is something we cannot
        # deliver.
        luma.append(("pulse", 16, BAR_V / 2.0, HAD_20T_US))
        chroma.append(("pulse", 16, BAR_V, HAD_20T_US))

    # 2.4 Five riser luminance staircase D1: transitions at 20, 22, 24,
    # 26, 28 and 31 (falling) H/32; peak to peak 0.700 V, so a step
    # height of 1/5 = 0.140 V nominal. See staircase_step() for what
    # becomes of that on an 8 bit grid.
    step = staircase_step(staircase_exact)
    for k in (20, 22, 24, 26, 28):
        luma.append(("step", k, step, HAD_STAIRCASE_US))
    luma.append(("step", 31, -5 * step, HAD_STAIRCASE_US))

    if chroma_on_staircase and not mono:
        # 2.4 Note: "Some administrations may wish to superimpose a
        # chrominance sub-carrier signal on this staircase. In this case,
        # the position and duration of the sub-carrier are determined by
        # instants 18H/32 and 31H/32. The other characteristics ... are
        # identical to those described in par. 4.3.2", so 0.280 V p-p.
        # OFF by default: J.63 par. 1 says that line 330 carries D2 and
        # line 17 carries D1, and Table II has line 17 measuring D1 and
        # line 330 measuring D2.
        chroma.append(("step", 18, 0.280, HAD_CHROMA_US, "D2", 2.0))
        chroma.append(("step", 31, -0.280, HAD_CHROMA_US, "", 0.0))
    return luma, chroma


def build_line18(mono=False):
    """Line 18, J.63 par. 3 and Figure 2."""
    if mono:
        # J.63 par. 1, note: with monochrome the pedestal and the
        # elements C1 and C2 fall away on line 18. Nothing is left.
        # (That same note does mention the pedestal + C1 + C2 as a
        # possible addition; do not switch --mono on then.)
        return [], []

    luma = [
        # 3.1 Luminance pedestal: transitions at 6H/32 and 31H/32, height
        # half of the bar = 0.350 V.
        ("step", 6, BAR_V / 2.0, HAD_EDGE_US),
        ("step", 31, -BAR_V / 2.0, HAD_EDGE_US),
        # 3.2 Reference bar C1: transitions at 6, 8 and 10 H/32; first
        # section 4/5 of the bar = 0.560 V, second section 1/5 = 0.140 V.
        # With respect to the pedestal of 0.350 V those are jumps of
        # +0.210, -0.420 and +0.210 V; those two extremes are exactly the
        # top and the trough of the multiburst below, and that is why C1
        # is there: it is the amplitude reference for C2.
        ("step", 6, 0.560 - 0.350, HAD_EDGE_US),
        ("step", 8, 0.140 - 0.560, HAD_EDGE_US),
        ("step", 10, 0.350 - 0.140, HAD_EDGE_US),
    ]

    # 3.3 Sine signals on the pedestal, C2. Starting positions and
    # frequencies from J.63 Table I. Peak to peak equal to that of C1,
    # 0.420 V, so +/- 0.210 V around the pedestal.
    for k, f in MULTIBURST:
        luma.append(("burst", k, f, 0.420 / 2.0))

    return luma, []


def build_line330(mono=False, staircase_exact=False):
    """Line 330, J.63 par. 4 and Figure 3."""
    # 4.1 and 4.2 are word for word the same as 2.1 and 2.2: the same bar
    # B2 and the same 2T pulse B1 as on line 17.
    luma = [
        ("step", 6, BAR_V, HAD_EDGE_US),
        ("step", 11, -BAR_V, HAD_EDGE_US),
        ("pulse", 13, BAR_V, HAD_2T_US),
    ]
    # 4.3.1 the same staircase D1 as on line 17.
    step = staircase_step(staircase_exact)
    for k in (20, 22, 24, 26, 28):
        luma.append(("step", k, step, HAD_STAIRCASE_US))
    luma.append(("step", 31, -5 * step, HAD_STAIRCASE_US))

    chroma = []
    if not mono:
        # 4.3.2 The colour subcarrier on the staircase turns it into D2:
        # "position and duration: 15H/32 to 30H/32" and "peak-to-peak
        # amplitude: 0,280 V +/- 2%".
        #
        # Note that this 15H/32 lies BEFORE the start of the staircase
        # (20H/32). Figure 3 draws it that way too: from 15 to 20 the
        # chroma packet sits at blanking level and the composite runs
        # from 0.16 to 0.44 V, so -20% to +20%. At the top the composite
        # ends up at 100 + 20 = 120%; that is in Figure 3 as well (scale
        # up to 1.14 V). Par. 4.3.2 allows the subcarrier to stop at
        # 28H/32; the main rule 15..30 is followed here.
        chroma.append(("step", 15, 0.280, HAD_CHROMA_US, "D2", 2.0))
        chroma.append(("step", 30, -0.280, HAD_CHROMA_US, "", 0.0))
    return luma, chroma


def build_line331(mono=False, three_level=False):
    """Line 331, J.63 par. 5 and Figure 4."""
    if mono:
        # J.63 par. 1, note: with monochrome the pedestal and the
        # elements G and E fall away on line 331.
        return [], []

    # 5.1 Luminance pedestal: transitions 6H/32 and 31H/32, 0.350 V.
    luma = [
        ("step", 6, BAR_V / 2.0, HAD_EDGE_US),
        ("step", 31, -BAR_V / 2.0, HAD_EDGE_US),
    ]

    chroma = []
    if three_level:
        # 5.3 Three level chroma signal G2, alternative to G1:
        # transitions at 7, 9, 11 and 14 H/32; peak to peak 1/5, 3/5 and
        # 5/5 of the bar = 0.140, 0.420 and 0.700 V.
        for k, v, name in ((7, 0.140, "G2a"), (9, 0.420 - 0.140, "G2b"),
                           (11, 0.700 - 0.420, "G2c"), (14, -0.700, "")):
            chroma.append(("step", k, v, HAD_CHROMA_US, name, 1.0))
    else:
        # 5.2 Chroma bar G1: transitions at 7H/32 and 14H/32, peak to
        # peak equal to the bar, 0.700 V.
        chroma.append(("step", 7, 0.700, HAD_CHROMA_US, "G1", 1.0))
        chroma.append(("step", 14, -0.700, HAD_CHROMA_US, "", 0.0))

    # 5.4 Reference subcarrier E: transitions at 17H/32 and 30H/32, peak
    # to peak 3/5 of the bar = 0.420 V.
    chroma.append(("step", 17, 0.420, HAD_CHROMA_US, "E", 1.0))
    chroma.append(("step", 30, -0.420, HAD_CHROMA_US, "", 0.0))
    return luma, chroma


# ---------------------------------------------------------------------
# THE MULTIBURST
# ---------------------------------------------------------------------

# J.63 Table I: starting position and frequency of the six bursts.
MULTIBURST = ((12, 0.5), (15, 1.0), (18, 2.0), (21, 4.0), (24, 4.8), (27, 5.8))

# Table I note (1): "The starting point of each burst shall be at zero
# phase of the sine-wave, and each burst shall consist of the maximum
# number of complete cycles. The gaps between successive bursts shall not
# be shorter than 0.4 us nor longer than 2.0 us in duration."
GAP_MIN_US, GAP_MAX_US = 0.4, 2.0
BURST_SLOT_US = 3 * H32_US  # the starts lie 3H/32 apart: 6 us


def burst_duration(f_mhz):
    """Duration of a burst: as many whole periods as fit.

    The gaps have to lie between 0.4 and 2.0 us and the starts lie 6 us
    apart, so the burst may last 5.6 us at most. The sixth burst has no
    successor; it gets the same rule here, because J.63 leaves the end of
    burst 6 open and Figure 2 draws it up to 30H/32, 6 us after its
    start, just like the other five.
    """
    max_duration = BURST_SLOT_US - GAP_MIN_US
    cycles = int(math.floor(max_duration * f_mhz + 1e-9))
    duration = cycles / f_mhz
    gap = BURST_SLOT_US - duration
    assert GAP_MIN_US - 1e-9 <= gap <= GAP_MAX_US + 1e-9, \
        "gap of %.3f us at %.1f MHz falls outside 0.4..2.0 us" % (gap, f_mhz)
    return duration, cycles


# ---------------------------------------------------------------------
# COMPUTING
# ---------------------------------------------------------------------


def eval_elements(elements, t):
    """Adds the elements up into a waveform at the instants t (us after 0H)."""
    y = np.zeros_like(t)
    for e in elements:
        kind = e[0]
        if kind == "step":
            y += e[2] * sin2_step(t, instant(e[1]), e[3])
        elif kind == "pulse":
            y += e[2] * sin2_pulse(t, instant(e[1]), e[3])
        elif kind == "burst":
            k, f, amp = e[1], e[2], e[3]
            t0 = instant(k)
            duration, _ = burst_duration(f)
            inside = (t >= t0) & (t < t0 + duration)
            y += np.where(inside, amp * np.sin(2.0 * np.pi * f * (t - t0)), 0.0)
        else:
            raise ValueError("unknown element %r" % (kind,))
    return y


def to_luma(volt):
    """Volts above blanking -> BT.601 luma value (unrounded)."""
    return Y_BLACK + Y_SPAN * (volt / BAR_V)


def to_cbcr(pp_volt):
    """Peak to peak amplitude of the colour subcarrier (volts) -> Cb, Cr (unrounded).

    HERE IS THE TRANSLATION FROM COMPOSITE TO YCbCr. In composite a
    chroma element is given as the peak to peak amplitude of the packet
    of 4.43 MHz. We do not deliver that packet; we deliver the Cb/Cr pair
    out of which the encoder makes it. The way back:

      BT.470-6 item 2.13: the peak amplitude of the colour subcarrier is
      G = sqrt(U^2 + V^2), expressed in the luminance range between
      blanking and peak white (note 14), so in units of 0.700 V. A peak
      to peak amplitude of pp volts therefore belongs to G = pp/(2*0.700).

      J.63 par. 1 fixes the phase at 60 degrees from the +(B-Y) axis, and
      +(B-Y) is the +U axis, so U = G*cos(60), V = G*sin(60).

      BT.470-6 item 2.5: U = 0.493*(B-Y) and V = 0.877*(R-Y).
      BT.601-7 par. 2.5.2/2.5.3: Cb = 224*(B-Y)/1.772 + 128 and
      Cr = 224*(R-Y)/1.402 + 128.

    The PAL switch (the sign of V per line) is NOT in here: the encoder
    makes that itself, and Cb/Cr are by definition the unswitched colour
    difference signals.

    QUANTISATION. Cb and Cr are simply rounded as BT.601-7 par. 2.5.3
    prescribes. At small amplitudes that costs noticeably: the first bit
    of the three level signal G2 (0.140 V p-p) comes out 1.41 percent too
    high because of it, while J.63 par. 5.3 allows 1 percent on it. The
    check at the bottom reports that as well. It can be repaired by
    picking, per sample, from the four roundings of (Cb, Cr) that lie
    closest to the requested vector, but that is no longer BT.601
    rounding then and it is only needed for the G2 variant, which J.63
    itself offers as an alternative. That is why it is not in here and
    why it is reported.
    """
    g = pp_volt / (2.0 * BAR_V)
    u = g * math.cos(math.radians(CHROMA_PHASE_DEG))
    v = g * math.sin(math.radians(CHROMA_PHASE_DEG))
    cb = C_ZERO + C_SPAN * (u / KU_PAL) / KB_601
    cr = C_ZERO + C_SPAN * (v / KV_PAL) / KR_601
    return cb, cr


OVERSAMPLING = 32


def sample_waveform(elements, n_out, aa=True, factor=OVERSAMPLING):
    """Samples a waveform at n_out points, spread over the 720 samples.

    For luma n_out is 720 (13.5 MHz), for chroma 360 (6.75 MHz).

    WHY THERE IS AN ANTIALIAS FILTER IN IT. The waveforms of J.63 are not
    exactly band limited. The 2T pulse has its first spectral zero at 5
    MHz but still has side lobes above that, and the multibursts are
    switched on and off hard: the standard itself says, Table I note (3),
    that "the out of band energy should be limited by suitable design
    techniques". Whoever samples such waveforms pointwise at 13.5 MHz
    folds everything above 6.75 MHz back into the band.

    That is why 32x oversampling is done here, cut off neatly at half the
    sampling frequency and then decimated. What comes out of that is the
    nearest version of the waveform from the standard that CAN be
    represented. The difference with the standard remains, you cannot get
    rid of that at 13.5 MHz, but it is now a neat band limitation instead
    of folded back rubbish at arbitrary frequencies.

    An FFT is allowed because the line begins and ends at blanking level
    on both sides: the periodic continuation makes no jump.
    """
    step = float(N_ACTIVE) / n_out  # in luma sample units: 1 or 2
    if not aa:
        return eval_elements(elements, t_of_sample(np.arange(n_out) * step))
    n = np.arange(n_out * factor) * (step / factor)
    fine = eval_elements(elements, t_of_sample(n))
    spec = np.fft.rfft(fine)
    spec[n_out // 2:] = 0  # everything from the Nyquist frequency of the output out
    return np.fft.irfft(spec, len(fine))[0::factor]


def build_line(luma_el, chroma_el, aa=True):
    """Computes an ITS line into 1440 bytes 4:2:2 (Cb,Y,Cr,Y).

    Luma at 13.5 MHz, 720 samples. Chroma at 6.75 MHz, 360 samples, and
    those are CO-SITED with the even luma samples, BT.656-5 par. 2.2:
    "the word sequence CB, Y, CR, refers to co-sited luminance and
    colour-difference samples". The chroma envelope is therefore computed
    analytically at those 360 instants and not decimated from the luma
    array; a transition at an odd instant (7H/32 falls on sample 57) then
    simply sits in its place and is not rounded to a chroma grid.
    """
    luma_v = sample_waveform(luma_el, N_ACTIVE, aa)
    chroma_pp = sample_waveform(chroma_el, N_ACTIVE // 2, aa) if chroma_el \
        else np.zeros(N_ACTIVE // 2)

    y = to_luma(luma_v)
    cb, cr = to_cbcr(chroma_pp)

    # BT.601-7 par. 2.5.3: int() rounds to the nearest value and rounds a
    # half upwards. numpy's rint rounds a half to even, so that is
    # floor(x + 0.5).
    yq = np.floor(y + 0.5).astype(np.int32)
    cbq = np.floor(cb + 0.5).astype(np.int32)
    crq = np.floor(cr + 0.5).astype(np.int32)

    # BT.656-5 par. 2.2: 0x00 and 0xFF are reserved for SAV/EAV and may
    # not occur in the picture data.
    for name, arr in (("luma", yq), ("Cb", cbq), ("Cr", crq)):
        if arr.min() < 1 or arr.max() > 254:
            raise ValueError("%s runs outside 1..254: %d..%d" % (name, arr.min(), arr.max()))

    buf = np.empty(N_ACTIVE * 2, dtype=np.uint8)
    buf[0::4] = cbq
    buf[1::4] = yq[0::2]
    buf[2::4] = crq
    buf[3::4] = yq[1::2]
    return buf


def from_bytes(buf):
    """Gets the luma (720) and Cb/Cr (360 each) back out of 1440 bytes."""
    y = np.empty(N_ACTIVE, dtype=np.float64)
    y[0::2] = buf[1::4]
    y[1::2] = buf[3::4]
    return y, buf[0::4].astype(np.float64), buf[2::4].astype(np.float64)


def back_to_volt(y):
    return (y - Y_BLACK) * BAR_V / Y_SPAN


def back_to_pp(cb, cr):
    """Cb/Cr -> peak to peak amplitude of the colour subcarrier, and the phase."""
    u = KU_PAL * KB_601 * (cb - C_ZERO) / C_SPAN
    v = KV_PAL * KR_601 * (cr - C_ZERO) / C_SPAN
    g = np.hypot(u, v)
    phase = np.degrees(np.arctan2(v, u))
    return 2.0 * BAR_V * g, phase


# ---------------------------------------------------------------------
# MEASURING
# ---------------------------------------------------------------------


def band_limited(y, factor=32):
    """Reconstructs the line the way an ideal filter would do it.

    A half amplitude duration of 200 ns is only 2.7 samples wide at 13.5
    MHz; you do not measure that on the sample grid, because then you
    measure the grid. The DAC and the output filter reconstruct a band
    limited signal, and that is what the pulse widths should be measured
    on. That is done here by putting zeros in the frequency domain, the
    exact band limited interpolation. That is allowed here: the line
    begins and ends at blanking level, so the periodic continuation makes
    no jump.
    """
    n = len(y)
    spec = np.fft.rfft(y - y[0])
    out = np.zeros(n * factor // 2 + 1, dtype=complex)
    out[:len(spec)] = spec
    # The Nyquist bin should be halved when upsampling.
    if n % 2 == 0:
        out[n // 2] *= 0.5
    return np.fft.irfft(out, n * factor) * factor + y[0]


def crossing(x, y, threshold, rising):
    """First place (in units of the x index) where y passes the threshold."""
    d = (y[:-1] < threshold) & (y[1:] >= threshold) if rising else \
        (y[:-1] > threshold) & (y[1:] <= threshold)
    idx = np.nonzero(d)[0]
    if len(idx) == 0:
        return None
    i = idx[0]
    frac = (threshold - y[i]) / (y[i + 1] - y[i])
    return x[i] + frac * (x[i + 1] - x[i])


def all_crossings(x, y, threshold):
    out = []
    for i in range(len(y) - 1):
        if (y[i] < threshold <= y[i + 1]) or (y[i] > threshold >= y[i + 1]):
            frac = (threshold - y[i]) / (y[i + 1] - y[i])
            out.append(x[i] + frac * (x[i + 1] - x[i]))
    return out


def measure_pulse(t, v, t_top_expected, search_us):
    """Peak position, peak amplitude and half amplitude duration of a pulse.

    The base is taken as the median of the edges of the search window, so
    that a pulse standing on a pedestal is measured properly too.
    """
    m = np.abs(t - t_top_expected) <= search_us
    tt, vv = t[m], v[m]
    edge = max(1, len(vv) // 20)
    base = 0.5 * (np.median(vv[:edge]) + np.median(vv[-edge:]))
    i = int(np.argmax(vv))
    peak = vv[i] - base
    half = base + peak / 2.0
    crs = all_crossings(tt, vv, half)
    had = (crs[-1] - crs[0]) if len(crs) >= 2 else float("nan")
    return tt[i], peak, had, base


def _burst_residual(t, v, f):
    """Residual of a least squares fit a*cos + b*sin + c at f."""
    w = 2.0 * np.pi * f * t
    M = np.column_stack([np.cos(w), np.sin(w), np.ones_like(w)])
    coef, *_ = np.linalg.lstsq(M, v, rcond=None)
    r = v - M.dot(coef)
    return float(r.dot(r)), math.hypot(coef[0], coef[1]), float(coef[2])


def measure_burst(t, v, f_mhz, t0, duration, cycles):
    """Frequency, amplitude and direct current level of a burst.

    The frequency is found by looking for the frequency at which a fit of
    a*cos + b*sin + c gives the smallest residual, over the inner part of
    the burst. First coarsely over +/- 2 percent, then finely.
    """
    margin = 0.5 / f_mhz  # half a period on either side: that is where the onset is
    m = (t >= t0 + margin) & (t <= t0 + duration - margin)
    tt, vv = t[m], v[m]

    best = f_mhz
    for width, steps in ((0.02, 161), (0.0004, 161)):
        grid = best * (1.0 + np.linspace(-width, width, steps))
        residuals = [_burst_residual(tt, vv, f)[0] for f in grid]
        best = float(grid[int(np.argmin(residuals))])

    # Amplitude and direct current at the NOMINAL frequency: that is the
    # amplitude of the signal that is supposed to be there.
    _, amp, dc = _burst_residual(tt, vv, f_mhz)
    return best, amp, dc


def flat_level(t, v, t0, t1, margin=0.35):
    """Mean level in the middle part of a flat region."""
    d = (t1 - t0) * margin
    m = (t >= t0 + d) & (t <= t1 - d)
    return float(np.mean(v[m])), float(np.std(v[m]))


# ---------------------------------------------------------------------
# THE CHECK
# ---------------------------------------------------------------------


# Everything that falls outside the tolerance of the standard ends up
# here, and is listed once more at the bottom. A check that only prints
# numbers proves nothing; there has to be a verdict with it.
OUT_OF_SPEC = []


def verdict(what, measured, norm, tol_pct, unit="V"):
    """Compares a measurement with the standard and keeps track of what falls outside."""
    dev = 100.0 * (measured - norm) / norm if norm else float("nan")
    if abs(dev) <= tol_pct:
        return "%.4f %s (%+.2f%%, standard %.3f +/- %.1f%%)  ok" % (measured, unit, dev, norm, tol_pct)
    OUT_OF_SPEC.append("%s: %.4f %s, %+.2f%% while the standard allows %.1f%%"
                       % (what, measured, unit, dev, tol_pct))
    return "%.4f %s (%+.2f%%, standard %.3f +/- %.1f%%)  OUTSIDE STANDARD" % (measured, unit, dev, norm, tol_pct)


def verdict_abs(what, measured, norm, tol_abs, unit="us"):
    """The same, but with an absolute tolerance instead of a percentage one."""
    dev = measured - norm
    if abs(dev) <= tol_abs:
        return "%.4f %s (%+.4f, standard %.4f +/- %.4f)  ok" % (measured, unit, dev, norm, tol_abs)
    OUT_OF_SPEC.append("%s: %.4f %s, %+.4f %s while the standard allows %.4f"
                       % (what, measured, unit, dev, unit, tol_abs))
    return "%.4f %s (%+.4f, standard %.4f +/- %.4f)  OUTSIDE STANDARD" % (measured, unit, dev, norm, tol_abs)


def check_building_blocks():
    """Computes the building blocks themselves, before they are used anywhere."""
    print("building blocks")
    t = np.linspace(-3, 3, 600001)

    # sin^2 pulse: peak 1, half amplitude duration equal to the given had.
    p = sin2_pulse(t, 0.0, 0.200)
    crs = all_crossings(t, p, 0.5)
    print("  sin2 pulse had given 200.0 ns -> measured %.2f ns, peak %.6f, "
          "base at +/- %.1f ns" % (1000 * (crs[-1] - crs[0]), p.max(),
                                   1000 * (t[p > 0][-1])))
    assert abs(1000 * (crs[-1] - crs[0]) - 200.0) < 0.1

    # sin^2 step: 50 percent exactly at t=0, 10-90% rise time 0.964*had.
    s = sin2_step(t, 0.0, 1.0)
    t50 = crossing(t, s, 0.5, True)
    t10 = crossing(t, s, 0.1, True)
    t90 = crossing(t, s, 0.9, True)
    print("  sin2 step  50%% at %+.6f (has to be 0), 10-90%% = %.4f x had "
          "(Tektronix gives 0.964), full transition 2 x had"
          % (t50, t90 - t10))
    assert abs(t50) < 1e-4 and abs((t90 - t10) - 0.964) < 5e-4

    # The window: every instant k*H/32 falls on sample 27k - 132.
    errors = 0
    for k in range(0, 33):
        n = sample_of_instant(k)
        if abs(n - (27 * k - 132)) > 1e-9:
            errors += 1
    print("  window     instant k*H/32 -> sample 27k-132, %d deviations out of 33"
          % errors)
    assert errors == 0
    print("             6H/32 = %.1f us -> sample %.0f ; 31H/32 = %.1f us -> sample %.0f"
          % (instant(6), sample_of_instant(6), instant(31), sample_of_instant(31)))

    # The multibursts
    print("  multiburst duration and gaps (have to lie between 0.4 and 2.0 us):")
    for k, f in MULTIBURST:
        duration, cycles = burst_duration(f)
        print("             %3.1f MHz from %2dH/32 = %5.1f us: %2d whole periods, "
              "%.4f us long, gap %.4f us, %.2f samples per period"
              % (f, k, instant(k), cycles, duration, BURST_SLOT_US - duration, FS_LUMA_MHZ / f))
    print()


def support(elements, threshold):
    """From when to when does this waveform differ from zero? In us after 0H.

    Not estimated from the instants but COMPUTED: the waveform is sampled
    finely over the whole line from 0 to 64 us, and it is checked where
    it rises above the threshold. An estimate based on the widest
    transition would be needlessly gloomy: the widest transition (the
    chroma envelope, 2.07 us) simply does not belong to the last instant
    (31H/32, which is a staircase edge of 0.45 us).
    """
    t = np.arange(0.0, H_US, 0.001)
    v = eval_elements(elements, t)
    m = np.abs(v) > threshold
    if not m.any():
        return None, None
    idx = np.nonzero(m)[0]
    return t[idx[0]], t[idx[-1]]


def check_window(all_lines):
    """Shows that every waveform fits within the 720 samples."""
    print("does it fit in 720 samples")
    print("  The window runs from sample 0 = %.3f us to sample 719 = %.3f us after 0H."
          % (t_of_sample(0), t_of_sample(N_ACTIVE - 1)))
    print("  For comparison (BT.470-6 Table 1-1): the line blanking ends at")
    print("  10.5 us (sample %.2f) and the front porch begins at 62.5 us (sample %.2f);"
          % (10.5 * FS_LUMA_MHZ - WINDOW_OFFSET, 62.5 * FS_LUMA_MHZ - WINDOW_OFFSET))
    print("  so the digital window sticks out a little on both sides, as it should.")

    # Everything below half a luma code no longer changes the output.
    threshold = 0.5 * BAR_V / Y_SPAN
    print("  threshold: %.2f mV = half a luma code; below that no byte changes"
          % (1000 * threshold))
    ok = True
    for nr, luma_el, chroma_el, _ in all_lines:
        for name, el in (("luma", luma_el), ("chroma", chroma_el)):
            if not el:
                continue
            t0, t1 = support(el, threshold)
            n0, n1 = t0 * FS_LUMA_MHZ - WINDOW_OFFSET, t1 * FS_LUMA_MHZ - WINDOW_OFFSET
            fits = (n0 >= 0) and (n1 <= N_ACTIVE - 1)
            ok = ok and fits
            print("  line %3d %-6s: %6.3f .. %6.3f us = sample %6.1f .. %6.1f  "
                  "-> clearance %5.1f in front, %5.1f behind  %s"
                  % (nr, name, t0, t1, n0, n1, n0, N_ACTIVE - 1 - n1,
                     "FITS" if fits else "DOES NOT FIT"))
    assert ok, "a waveform falls outside the 720 samples"
    print()


def check_line(nr, buf, luma_el, chroma_el):
    """Measures back in the OUTGOING BYTES what is supposed to be in them."""
    print("line %d" % nr)
    y8, cb8, cr8 = from_bytes(buf)
    v = back_to_volt(y8)
    pp, phase = back_to_pp(cb8, cr8)

    n = np.arange(N_ACTIVE, dtype=np.float64)
    t = t_of_sample(n)
    tc = t_of_sample(n[0::2])

    # band limited reconstruction: the narrow things are measured on this
    fac = 32
    vr = back_to_volt(band_limited(y8, fac))
    tr = t_of_sample(np.arange(N_ACTIVE * fac, dtype=np.float64) / fac)

    kinds = set(e[0] for e in luma_el)
    ks = set(e[1] for e in luma_el)

    if 6 in ks and 11 in ks and any(e[1] == 11 and e[2] < -0.5 for e in luma_el if e[0] == "step"):
        # ---- B2 bar --------------------------------------------------
        # J.63 par. 2.1: amplitude 0.700 +/- 0.007 V (= +/- 1%),
        # transitions at 6H/32 and 11H/32, overshoot and tilt <= 0.5%.
        lev, sd = flat_level(t, v, instant(6), instant(11))
        rise = crossing(tr, vr, lev / 2, True)
        fall = crossing(tr[tr > instant(8)], vr[tr > instant(8)], lev / 2, False)
        t10 = crossing(tr, vr, 0.1 * lev, True)
        t90 = crossing(tr, vr, 0.9 * lev, True)
        m = (t >= instant(6) + 0.5) & (t <= instant(11) - 0.5)
        tilt = 100 * (v[m].max() - v[m].min()) / BAR_V
        # overshoot and undershoot right next to the edges, J.63 par. 2.1: 0.5%
        mo = ((tr >= instant(6) - 1.0) & (tr <= instant(6) + 1.0)) |              ((tr >= instant(11) - 1.0) & (tr <= instant(11) + 1.0))
        over = 100 * max(vr[mo].max() - lev, -vr[mo].min()) / BAR_V
        print("  B2 bar       %s" % verdict("B2 bar line %d" % nr, lev, BAR_V, 1.0))
        print("               50%% edge up   %s"
              % verdict_abs("B2 rising edge line %d" % nr, rise, instant(6), 0.250))
        print("               50%% edge down %s"
              % verdict_abs("B2 falling edge line %d" % nr, fall, instant(11), 0.250))
        print("               10-90%% = %.0f ns (2T shaping gives 0.964 x 200 = 193 ns)"
              % (1000 * (t90 - t10)))
        print("               flatness of the top %.2f%%, over-/undershoot at the "
              "edges %.2f%% (standard 0.5%%)  %s"
              % (tilt, over, "ok" if max(tilt, over) <= 0.5 else "OUTSIDE STANDARD"))
        if max(tilt, over) > 0.5:
            OUT_OF_SPEC.append("B2 flatness/overshoot line %d: %.2f%% while the standard allows 0.5%%"
                               % (nr, max(tilt, over)))

    if any(e[0] == "pulse" and e[1] == 13 for e in luma_el):
        # ---- B1 2T pulse ---------------------------------------------
        # J.63 par. 2.2: peak at 13H/32, amplitude within 1% of the bar,
        # half amplitude duration 200 +/- 10 ns.
        tp, amp, had, _ = measure_pulse(tr, vr, instant(13), 0.9)
        print("  B1 2T pulse  amplitude %s" % verdict("B1 amplitude line %d" % nr, amp, BAR_V, 1.0))
        print("               width at half height %s"
              % verdict_abs("B1 half amplitude duration line %d" % nr, 1000 * had, 200.0, 10.0, "ns"))
        print("               peak at %s"
              % verdict_abs("B1 peak position line %d" % nr, tp, instant(13), 0.250))

    if any(e[0] == "pulse" and e[1] == 16 for e in luma_el):
        # ---- F 20T pulse ---------------------------------------------
        # In composite the top is luma + chroma_pp/2 and the bottom
        # luma - chroma_pp/2. Both are reconstructed here from what is
        # really in the bytes. J.63 par. 2.3: peak at 16H/32, amplitude
        # within 1% of the bar, half amplitude duration 2 +/- 0.06 us,
        # disturbance of the base <= 0.5% of the peak amplitude.
        pp_on_luma = np.interp(t, tc, pp)
        top, bottom = v + pp_on_luma / 2, v - pp_on_luma / 2
        fac2 = 8
        tb = t_of_sample(np.arange(N_ACTIVE * fac2, dtype=np.float64) / fac2)
        bb = np.interp(tb, t, top)
        tp, amp, had, _ = measure_pulse(tb, bb, instant(16), 2.5)
        m = (t >= instant(15) - 0.2) & (t <= instant(17) + 0.2)
        base = float(np.max(np.abs(bottom[m])))
        print("  F 20T pulse  amplitude %s" % verdict("F amplitude line %d" % nr, amp, BAR_V, 1.0))
        print("               width at half height %s"
              % verdict_abs("F half amplitude duration line %d" % nr, had, 2.0, 0.06))
        print("               peak at %s"
              % verdict_abs("F peak position line %d" % nr, tp, instant(16), 0.250))
        print("               luma part %s" % verdict("F luma part line %d" % nr,
                                                     float(np.max(v[m])), 0.350, 1.0))
        print("               chroma p-p %s" % verdict("F chroma line %d" % nr,
                                                        float(np.max(pp_on_luma[m])), 0.700, 1.0))
        flat = 100 * base / BAR_V
        print("               base flat within %.2f%% of the peak (standard 0.5%%)  %s"
              % (flat, "ok" if flat <= 0.5 else "OUTSIDE STANDARD"))
        if flat > 0.5:
            OUT_OF_SPEC.append("F base line %d: %.2f%% while the standard allows 0.5%%" % (nr, flat))

    if 20 in ks:
        # ---- D1 staircase --------------------------------------------
        # J.63 par. 2.4 / 4.3.1: transitions at 20, 22, 24, 26, 28 and 31
        # H/32, peak to peak within 1% of the bar, step height 1/5 =
        # 0.140 V, and the difference between the largest and the
        # smallest step less than 0.5% of the largest.
        bounds = [20, 22, 24, 26, 28, 31]
        levels = []
        for i in range(5):
            lev, _ = flat_level(t, v, instant(bounds[i]), instant(bounds[i + 1]))
            levels.append(lev)
        steps = [levels[0]] + [levels[i] - levels[i - 1] for i in range(1, 5)]
        spread = 100 * (max(steps) - min(steps)) / max(steps)
        print("  D1 staircase levels %s V" % " ".join("%.4f" % x for x in levels))
        print("               steps   %s V" % " ".join("%.4f" % x for x in steps))
        print("               step height nominal %.4f V (%.1f luma codes)"
              % (steps[0], steps[0] * Y_SPAN / BAR_V))
        print("               peak to peak %s" % verdict("D1 top line %d" % nr, levels[-1], BAR_V, 1.0))
        print("               largest-smallest step %.2f%% (standard 0.5%%)  %s"
              ", see the note about 8 bits at the bottom"
              % (spread, "ok" if spread <= 0.5 else "OUTSIDE STANDARD"))
        if spread > 0.5:
            OUT_OF_SPEC.append("D1 step equality line %d: %.2f%% while the standard allows 0.5%%"
                               % (nr, spread))

    if 8 in ks:
        # ---- pedestal and C1 -----------------------------------------
        # J.63 par. 3.1: pedestal 0.350 V within 1%. Par. 3.2: C1
        # sections 0.560 and 0.140 V, each within 1%.
        ped, _ = flat_level(t, v, instant(10), instant(12))
        s1, _ = flat_level(t, v, instant(6), instant(8))
        s2, _ = flat_level(t, v, instant(8), instant(10))
        print("  pedestal     %s" % verdict("pedestal line %d" % nr, ped, 0.350, 1.0))
        print("  C1 section 1 %s" % verdict("C1 section 1 line %d" % nr, s1, 0.560, 1.0))
        print("  C1 section 2 %s" % verdict("C1 section 2 line %d" % nr, s2, 0.140, 1.0))
        print("  C1 p-p       %.4f V (standard 0.420), this is the amplitude reference for C2"
              % (s1 - s2))

    if any(e[0] == "burst" for e in luma_el):
        # ---- C2 multiburst -------------------------------------------
        # J.63 par. 3.3: peak to peak within 1% of that of C1 (0.420 V),
        # direct current component <= 0.5% of it. The standard has no
        # tolerance for the frequency itself; that simply has to be right.
        print("  C2 multiburst (measured on the band limited reconstruction):")
        for k, f in MULTIBURST:
            duration, cycles = burst_duration(f)
            fm, amp, dc = measure_burst(tr, vr, f, instant(k), duration, cycles)
            print("     %3.1f MHz  p-p %s" % (f, verdict("C2 %.1f MHz line %d" % (f, nr),
                                                        2 * amp, 0.420, 1.0)))
            print("               frequency %.4f MHz (%+.3f%%, %2d whole periods), "
                  "direct current %.4f V (standard 0.350, %+.2f%%, standard 0.5%%)"
                  % (fm, 100 * (fm - f) / f, cycles, dc,
                     100 * (dc - 0.350) / 0.350))
            if abs(100 * (fm - f) / f) > 0.5:
                OUT_OF_SPEC.append("C2 %.1f MHz line %d: frequency %+.3f%% off" % (f, nr, 100 * (fm - f) / f))

    flats = chroma_flats(chroma_el) if chroma_el else []
    if flats:
        # ---- chroma --------------------------------------------------
        print("  chroma (computed back from Cb/Cr):")
        for name, k0, k1, norm, tol in flats:
            lev, sd = flat_level(tc, pp, instant(k0), instant(k1))
            m = (tc >= instant(k0) + 1.5) & (tc <= instant(k1) - 1.5)
            f_mean = float(np.mean(phase[m])) if m.any() else float("nan")
            print("     %-4s %2dH/32..%2dH/32  p-p %s"
                  % (name, k0, k1, verdict("%s line %d" % (name, nr), lev, norm, tol)))
            print("                        phase %s"
                  % verdict_abs("%s phase line %d" % (name, nr), f_mean, 60.0, 5.0, "degrees"))
        # rise time of the envelope, on the first rising edge
        rising = [e for e in chroma_el if e[0] == "step" and e[2] > 0]
        if rising:
            k, delta = rising[0][1], rising[0][2]
            m = np.abs(tc - instant(k)) < 3.0
            t10 = crossing(tc[m], pp[m], 0.1 * delta, True)
            t90 = crossing(tc[m], pp[m], 0.9 * delta, True)
            t50 = crossing(tc[m], pp[m], 0.5 * delta, True)
            if t10 and t90 and t50:
                # J.63 par. 1: chroma instants may deviate by 500 ns.
                print("     envelope edge at %2dH/32: 50%% at %s"
                      % (k, verdict_abs("chroma edge %dH/32 line %d" % (k, nr),
                                        t50, instant(k), 0.500)))
                print("                                 10-90%% = %.3f us "
                      "(standard about 1 us)" % (t90 - t10))

    # ---- code range --------------------------------------------------
    print("  codes        luma %d..%d (studio range 16..235), Cb %d..%d, Cr %d..%d "
          "(studio range 16..240)"
          % (y8.min(), y8.max(), cb8.min(), cb8.max(), cr8.min(), cr8.max()))
    if y8.min() < 16 or y8.max() > 235:
        print("               outside 16..235, and that is allowed: BT.601-7 says that the")
        print("               signal \"may occasionally excurse beyond level 235.00d or below")
        print("               level 16.00d\". Only 0 and 255 are forbidden (BT.656-5).")
    print()


def chroma_flats(chroma_el):
    """Which flat chroma parts there are in this line, to be measured.

    Every rising step begins a flat part that runs until the next step.
    Name and tolerance are in the step itself, because those differ per
    element: D2 may deviate by 2 percent (J.63 par. 4.3.2), G1, G2 and E
    by only 1 percent (par. 5.2, 5.3 and 5.4).
    """
    steps = sorted([e for e in chroma_el if e[0] == "step"], key=lambda e: e[1])
    out, level = [], 0.0
    for i, e in enumerate(steps):
        level += e[2]
        if i + 1 < len(steps) and level > 1e-6 and len(e) > 4 and e[4]:
            out.append((e[4], e[1], steps[i + 1][1], level, e[5]))
    return out


def check_deviation(luma_el, factor=OVERSAMPLING):
    """How far is what we send from the waveform of the standard?

    The waveform of the standard is computed finely. Alongside that the
    line is sampled, once with the antialias filter and once pointwise,
    and reconstructed band limited again the way the DAC and the output
    filter do it. The difference with the fine waveform is what gets lost
    on the way.
    """
    n = np.arange(N_ACTIVE * factor) / float(factor)
    ideal = eval_elements(luma_el, t_of_sample(n))
    out = {}
    for aa in (True, False):
        rec = band_limited(sample_waveform(luma_el, N_ACTIVE, aa), factor)
        d = np.abs(ideal - rec)
        i = int(np.argmax(d))
        out[aa] = (float(d[i]), float(t_of_sample(n[i])))
    return out[True], out[False]


# ---------------------------------------------------------------------
# PICTURE
# ---------------------------------------------------------------------

# The standard draws composite: blanking at 0.30 V, peak white at 1.00 V,
# sync bottom at 0 V. That is imitated here, so that the picture can be
# laid next to Figure 1 to 4 of J.63. NOTE: this is a RECONSTRUCTION of
# what the encoder is going to make of it, not of what we send; we send
# only the centre line (luma) and the bandwidth (chroma envelope).
BLANK_V = 0.300


def draw(lines, path, width=1500, height_per=270):
    if Image is None:
        print("PIL missing, no PNG written")
        return
    margin_l, margin_r, margin_b, margin_t = 62, 18, 34, 26
    high = height_per * len(lines)
    im = Image.new("RGB", (width, high), (255, 255, 255))
    d = ImageDraw.Draw(im)

    pw = width - margin_l - margin_r
    v0, v1 = -0.05, 1.20  # vertical scale in composite volts

    for i, (nr, buf, _, _) in enumerate(lines):
        y_top = i * height_per + margin_t
        y_bot = (i + 1) * height_per - margin_b
        ph = y_bot - y_top

        def px(t_us):
            n = t_us * FS_LUMA_MHZ - WINDOW_OFFSET
            return margin_l + pw * n / float(N_ACTIVE - 1)

        def py(volt):
            return y_bot - ph * (volt - v0) / (v1 - v0)

        # grid: every instant k*H/32 that falls in the picture
        for k in range(0, 33):
            x = px(instant(k))
            if margin_l - 1 <= x <= width - margin_r + 1:
                colour = (150, 150, 150) if k % 2 == 0 else (215, 215, 215)
                d.line([(x, y_top), (x, y_bot)], fill=colour)
                if k % 2 == 0:
                    d.text((x - 5, y_bot + 3), str(k), fill=(90, 90, 90))
        # levels the way the standard puts them on the axis
        for volt, lab in ((0.30, "0,30"), (0.44, "0,44"), (0.58, "0,58"),
                          (0.72, "0,72"), (0.86, "0,86"), (1.00, "1,00"), (1.14, "1,14")):
            yy = py(volt)
            d.line([(margin_l, yy), (width - margin_r, yy)], fill=(230, 230, 230))
            d.text((6, yy - 6), lab, fill=(120, 120, 120))
        d.line([(margin_l, py(BLANK_V)), (width - margin_r, py(BLANK_V))], fill=(120, 120, 120))

        y8, cb8, cr8 = from_bytes(buf)
        v = back_to_volt(y8) + BLANK_V
        pp, _ = back_to_pp(cb8, cr8)
        ppl = np.interp(np.arange(N_ACTIVE), np.arange(0, N_ACTIVE, 2), pp)

        xs = [margin_l + pw * n / float(N_ACTIVE - 1) for n in range(N_ACTIVE)]
        # chroma as a band around the luma, just like the hatching in the standard
        if ppl.max() > 1e-4:
            for n in range(N_ACTIVE):
                if ppl[n] > 1e-4:
                    d.line([(xs[n], py(v[n] - ppl[n] / 2)), (xs[n], py(v[n] + ppl[n] / 2))],
                           fill=(210, 225, 245))
            d.line([(xs[n], py(v[n] + ppl[n] / 2)) for n in range(N_ACTIVE)], fill=(70, 120, 190))
            d.line([(xs[n], py(v[n] - ppl[n] / 2)) for n in range(N_ACTIVE)], fill=(70, 120, 190))
        d.line([(xs[n], py(v[n])) for n in range(N_ACTIVE)], fill=(0, 0, 0))
        d.text((margin_l + 4, y_top - 15), "line %d   (x H/32 after 0H; sample 0 = 9.778 us)" % nr,
               fill=(0, 0, 0))

    im.save(path)
    print("%s written (%dx%d)" % (path, width, high))


# ---------------------------------------------------------------------


def write_output(lines, outdir):
    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    head = ["// Generated by tools/make_its.py. Do not edit by hand.",
            "// Insertion test signals to ITU-T J.63 (formerly ITU-R CMTT.473),",
            "// Annex I, 625 line systems. Per line 720 pixels in 4:2:2 (Cb,Y,Cr,Y),",
            "// ready to be sent as the active part of the picture line in question.",
            "// The docstring of the tool says where every number comes from.",
            "#pragma once",
            "",
            "#include <stdint.h>",
            ""]
    nrs = []
    for nr, buf, _, _ in lines:
        nrs.append(nr)
        with open(os.path.join(outdir, "its_line%d.bin" % nr), "wb") as f:
            f.write(buf.tobytes())
        head.append("static const uint8_t ITS_LINE%d[%d] = {" % (nr, len(buf)))
        for i in range(0, len(buf), 16):
            head.append("    " + " ".join("0x%02X," % b for b in buf[i:i + 16]))
        head.append("};")
        head.append("")
    head.append("// The four ITS line numbers in the 625 line count of BT.1700.")
    head.append("static const int ITS_LINES[%d] = {%s};" % (len(nrs), ", ".join(str(n) for n in nrs)))
    head.append("static const uint8_t *const ITS_DATA[%d] = {%s};"
                % (len(nrs), ", ".join("ITS_LINE%d" % n for n in nrs)))
    head.append("")
    path = os.path.join(outdir, "its_data.h")
    with open(path, "w") as f:
        f.write("\n".join(head))
    print("%s written (%d lines)" % (path, len(lines)))


def main():
    p = argparse.ArgumentParser(description="ITS/VITS for 625/50 to ITU-T J.63")
    p.add_argument("--png", help="write the four waveforms as a PNG")
    p.add_argument("--outdir", default=OUT_DIR, help="where .h and .bin go")
    p.add_argument("--mono", action="store_true",
                   help="monochrome base variant (J.63 Annex I par. 1, note)")
    p.add_argument("--g2", action="store_true",
                   help="line 331 with the three level signal G2 instead of G1")
    p.add_argument("--staircase-exact", action="store_true",
                   help="staircase exactly 0.700 V p-p, at the cost of equal steps")
    p.add_argument("--chroma-on-staircase", action="store_true",
                   help="line 17 with colour subcarrier on the staircase (J.63 par. 2.4, note)")
    p.add_argument("--no-output", action="store_true", help="only compute and measure")
    p.add_argument("--pointwise", action="store_true",
                   help="no antialias filter: sample the waveform pointwise")
    a = p.parse_args()

    check_building_blocks()

    builds = [
        (17, build_line17(a.mono, a.chroma_on_staircase, a.staircase_exact)),
        (18, build_line18(a.mono)),
        (330, build_line330(a.mono, a.staircase_exact)),
        (331, build_line331(a.mono, a.g2)),
    ]
    all_lines = []
    for nr, (luma_el, chroma_el) in builds:
        if not luma_el and not chroma_el:
            print("line %d: empty in this variant (J.63 Annex I par. 1, note)\n" % nr)
            continue
        all_lines.append((nr, luma_el, chroma_el, None))

    check_window(all_lines)

    lines = []
    for nr, luma_el, chroma_el, _ in all_lines:
        buf = build_line(luma_el, chroma_el, aa=not a.pointwise)
        lines.append((nr, buf, luma_el, chroma_el))

    for nr, buf, luma_el, chroma_el in lines:
        check_line(nr, buf, luma_el, chroma_el)

    print("what gets lost between the standard and what goes out of the door")
    print("  (largest deviation from the finely computed waveform, after")
    print("   sampling and reconstructing band limited again):")
    for nr, _, luma_el, _ in lines:
        (with_aa, tm), (without_aa, tz) = check_deviation(luma_el)
        print("  line %3d: with antialias filter %.4f V = %5.2f%% of 0.700 = %5.2f luma code, "
              "at %.2f us (%.1f H/32)"
              % (nr, with_aa, 100 * with_aa / BAR_V, with_aa * Y_SPAN / BAR_V, tm, tm / H32_US))
        print("            pointwise that would be %.4f V = %5.2f%% = %5.2f luma code, at %.2f us"
              % (without_aa, 100 * without_aa / BAR_V, without_aa * Y_SPAN / BAR_V, tz))
    print()

    print("quantisation: 1 luma code = %.4f mV = %.3f%% of the bar"
          % (1000 * BAR_V / Y_SPAN, 100.0 / Y_SPAN))
    print("  The staircase: 219/5 = 43.8 codes is not a whole number, so steps of exactly")
    print("  1/5 of the bar always differ by 1 code = 2.3 percent, well outside the")
    print("  0.5 percent that J.63 par. 2.4 allows. Ten bits do not help either")
    print("  (876/5 = 175.2 -> 0.57 percent). Therefore %s."
          % ("steps of exactly 44 codes are chosen: equal down to the code, "
             "peak to peak +0.46 percent (within the 1 percent of J.63) and the top at "
             "code 236, one code above peak white"
             if not a.staircase_exact else
             "with --staircase-exact a peak to peak of exactly 0.700 V is chosen, "
             "and then the steps are 44/44/43/44/44"))
    print()

    if OUT_OF_SPEC:
        print("OUTSIDE THE STANDARD (%d):" % len(OUT_OF_SPEC))
        for r in OUT_OF_SPEC:
            print("  " + r)
    else:
        print("everything within the tolerance of J.63")
    print()

    if a.png:
        draw(lines, a.png)
    if not a.no_output:
        write_output(lines, a.outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())

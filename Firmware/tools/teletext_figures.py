"""Measures figures 2 and 3 of ETS 300 706 instead of looking at them.

    python tools/teletext_figures.py

Why this exists: clause 5.4 of ETS 300 706 says about the shape of the
data pulse only that the spectrum is "skew symmetrical about 0,5 x bit
rate, substantially zero by 5 MHz", and refers for the rest to figures 2
and 3. There is not a single number with them. "Substantially zero by
5 MHz" cannot be read as a roll-off, and it is exactly that which
determines how hard the pulse rings, and with it how far the waveform
dips below black.

Fortunately both figures are VECTOR drawings in the pdf. The curve is in
there as a series of line segments, and the axes are as well, so you can
read it out point by point and calibrate it against the axes. That is
exactly what happens here. The result is in src/teletext.cpp at
TT_ALPHA.

The pdf is NOT in the repository. Get it from ETSI and put it down as
datasheets/ets_300706e01p.pdf:

    https://www.etsi.org/deliver/etsi_i_ets/300700_300799/300706/01_60/ets_300706e01p.pdf
"""

import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PDF = os.path.join(os.path.dirname(ROOT), "datasheets", "ets_300706e01p.pdf")

R = 6.9375  # bit rate in MHz, ETS 5.3

# CALIBRATION POINTS, read off from the drawing objects of the pdf (page
# 15 of the standard = pdf page 15). They are the axes and the tick marks
# themselves, not the labels: those are next to them with a text offset.
#
# Figure 2 (the pulse):  x axis 0 at 197.52 pt with ticks at -2, -1 and 2;
#                        y = 0 on the axis at 471.70 and y = 1.0 at 355.20.
# Figure 3 (spectrum):   y axis at 348.84 = 0 MHz, 1 MHz per 23.03 pt;
#                        y = 0 at 477.48 and y = 1.0 at 355.80.
FIG2 = dict(page=14, objects=(3, 4),
            xpt=(115.80, 156.84, 197.52, 279.24), xvalue=(-2.0, -1.0, 0.0, 2.0),
            y0=471.70, y1=355.20)
FIG3 = dict(page=14, objects=(24,),
            xpt=(348.84, 371.87, 394.90), xvalue=(0.0, 1.0, 2.0),
            y0=477.48, y1=355.80)


def extract_curve(doc, spec):
    p = doc[spec["page"]]
    drawings = p.get_drawings()
    pts = []
    for k in spec["objects"]:
        for it in drawings[k]["items"]:
            if it[0] == "l":
                pts.append((it[1].x, it[1].y))
                pts.append((it[2].x, it[2].y))
    pts = np.array(pts)
    a, b = np.polyfit(spec["xpt"], spec["xvalue"], 1)
    x = a * pts[:, 0] + b
    y = (spec["y0"] - pts[:, 1]) / (spec["y0"] - spec["y1"])
    o = np.argsort(x)
    x, y = x[o], y[o]
    xu = np.unique(np.round(x, 4))
    yu = np.array([y[np.round(x, 4) == v].mean() for v in xu])  # average the line thickness
    return xu, yu


def rc_spectrum(f, alpha):
    f = np.abs(np.asarray(f, dtype=float))
    lo, hi = (1 - alpha) * R / 2, (1 + alpha) * R / 2
    out = np.where(f <= lo, 1.0, 0.0)
    m = (f > lo) & (f < hi)
    return np.where(m, 0.5 * (1 + np.cos(np.pi / (alpha * R) * (f - lo))), out)


def rc_pulse(x, alpha):
    x = np.asarray(x, dtype=float)
    n = 1.0 - (2 * alpha * x) ** 2
    safe = np.where(np.abs(n) < 1e-9, 1.0, n)
    r = np.sinc(x) * np.cos(np.pi * alpha * x) / safe
    return np.where(np.abs(n) < 1e-9, (np.pi / 4) * np.sinc(x), r)


def main():
    try:
        import pymupdf
    except ImportError:
        raise SystemExit("pip install pymupdf")
    if not os.path.exists(PDF):
        raise SystemExit("not found: %s\nSee the head of this file." % PDF)
    doc = pymupdf.open(PDF)

    print("=== figure 3: spectrum of the data pulse ===")
    f, A = extract_curve(doc, FIG3)
    mid = float(np.interp(R / 2, f, A))
    print("at half the bit rate (%.5f MHz) the curve reads %.4f." % (R / 2, mid))
    print("Skew symmetry requires 0.5 there; that is right then, and with it the calibration.")
    print()
    print("  MHz      drawn")
    for fi in (1.0, 2.0, 3.0, R / 2, 4.0, 4.5, 5.0, 5.5, 6.0):
        print("%5.2f   %8.4f" % (fi, np.interp(fi, f, A)))
    for lim in (0.10, 0.05, 0.02):
        i = np.nonzero(A < lim)[0]
        if len(i):
            print("drops below %.2f at %.2f MHz" % (lim, f[i[0]]))
    best = min(((a, ((A - rc_spectrum(f, a)) ** 2).mean())
                for a in np.linspace(0.30, 0.99, 700)), key=lambda t: t[1])
    print()
    print("best fitting raised cosine: alpha = %.3f (rms %.4f), zero at %.2f MHz"
          % (best[0], np.sqrt(best[1]), (1 + best[0]) * R / 2))
    print("for comparison: alpha = %.3f would be 'exactly zero at 5.0 MHz'"
          % (2 * 5.0 / R - 1))

    print()
    print("=== figure 2: the pulse of one bit ===")
    x, y = extract_curve(doc, FIG2)
    print("peak %.4f at x = %+.3f bit period" % (y.max(), x[y.argmax()]))
    print("deepest undershoot %.4f at x = %+.3f" % (y.min(), x[y.argmin()]))
    print()
    print("     x      drawn   RC 0.44   RC 0.60   RC 0.75   rectangle*RC")
    for xi in (0.5, 1.0, 1.25, 1.5, 2.0):
        u = np.linspace(-0.5, 0.5, 2001)
        rect = (rc_pulse(xi - u, 0.6) * (u[1] - u[0])).sum()
        print("%6.2f   %8.4f  %8.4f  %8.4f  %8.4f  %12.4f"
              % (xi, np.interp(xi, x, y), rc_pulse(xi, 0.441441), rc_pulse(xi, 0.60),
                 rc_pulse(xi, 0.75), rect))
    print()
    print("The drawn pulse goes through ZERO at x = 1 and x = 2. That makes it a")
    print("Nyquist pulse, and rules out the filtered rectangle: at x = 1 that is")
    print("still above zero. The undershoot belongs with a roll-off between 0.65")
    print("and 0.75, while the spectrum of figure 3 comes out at 0.59. Both")
    print("figures are called 'Approximate' in the standard. src/teletext.cpp")
    print("takes 0.60, because clause 5.4 is about the spectrum.")


if __name__ == "__main__":
    main()

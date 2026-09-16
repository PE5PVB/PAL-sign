"""
Lays the C# conversion next to the Python conversion.

    python compare.py

WHY THIS EXISTS. The configurator is the SECOND implementation of the
image conversion; the first is in Firmware/tools/convert_image.py and is
the one proven to work on the board. Two implementations drift apart
sooner or later, and then a difference on the screen can no longer be
traced back to a cause. This test makes that measurable instead of us
hoping.

TWO TRIALS, and the distinction is the whole point:

  A. THE ARITHMETIC, on input that is already 720x576. Nothing is scaled
     at all then, so all that is left is the YCbCr conversion, the
     flicker filter, the chroma halving and the level limiting. That
     ought to be equal BYTE FOR BYTE: there is no room for a difference
     of taste in it. If not, there is a real fault.

  B. THE SCALING, on a photo that does have to be scaled. No equality is
     expected here: PIL works out its Lanczos weights in fixed point and
     rounds per pass to 8 bits, the C# version works in double. What is
     measured here is how BIG the difference is, so that you know whether
     it is rounding noise or something else.

BEFORE EITHER TRIAL: ALGO_VERSION in convert_image.py and
ImageConvert.AlgoVersion in the C# are checked against each other. A
byte-for-byte pass in trial A only proves the two are still the same
algorithm if this matched first; without it, a change to one side that
happens to still pass on this particular random input would go
unnoticed.
"""

import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "Firmware", "tools"))
import convert_image as ci                              # noqa: E402

EXE = os.path.join(HERE, "src", "bin", "Debug", "net8.0-windows", "PAL-sign.exe")


def csharp(path, out, flicker=1.0, fill=False, square=False):
    args = [EXE, "--dump", path, out, "--flicker", "%.4f" % flicker]
    if fill:
        args.append("--fill")
    if square:
        args.append("--square")
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0 or not os.path.exists(out):
        err = out + ".error.txt"
        message = open(err).read() if os.path.exists(err) else r.stderr.decode("mbcs", "replace")
        raise SystemExit("C# returned %d:\n%s" % (r.returncode, message))
    return np.fromfile(out, np.uint8).reshape(576, 1440)


def csharp_algo_version():
    r = subprocess.run([EXE, "--algo-version"], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("C# --algo-version returned %d:\n%s" % (r.returncode, r.stderr))
    return int(r.stdout.strip())


def python_side(path, flicker=1.0, fill=False, square=False):
    im = Image.open(path).convert("RGB")
    canvas = ci.fit_in(im, fill, square)
    return ci.to_422(np.array(canvas), 0.0, flicker)


def difference(a, b):
    d = np.abs(a.astype(int) - b.astype(int))
    return d.max(), d.mean(), int((d > 0).sum()), d.size


def main():
    if not os.path.exists(EXE):
        raise SystemExit("build first:  dotnet build src/PAL-sign.csproj\n(not found: %s)" % EXE)

    # Checked before a single pixel is compared: a byte-for-byte pass
    # below only proves the two are the same algorithm if this matches.
    # See ALGO_VERSION in convert_image.py and ImageConvert.AlgoVersion.
    cs_version = csharp_algo_version()
    if cs_version != ci.ALGO_VERSION:
        raise SystemExit(
            "ALGO_VERSION mismatch: convert_image.py is %d, ImageConvert.cs is %d.\n"
            "One of the two implementations changed without the other; bump "
            "whichever is behind to match, in the same commit as the change "
            "that caused this, only once the pixel trials below pass again."
            % (ci.ALGO_VERSION, cs_version))

    tmp = os.path.join(HERE, "compare_tmp.bin")
    faults = 0

    # --- A. the arithmetic, without scaling -------------------------
    print("A. ARITHMETIC: input is already 720x576, so nothing is scaled.")
    print("   This ought to be equal byte for byte.\n")
    trial = os.path.join(HERE, "compare_trial.png")
    rng = np.random.default_rng(20250811)
    # Random, so that every colour and level combination occurs, plus
    # hard edges that put the flicker filter to work.
    picture = rng.integers(0, 256, (576, 720, 3), dtype=np.uint8)
    picture[::2, :, :] = 255          # every other row white: maximum twitter
    picture[1::2, :, :] = 0
    picture[:, 300:420, :] = rng.integers(0, 256, (576, 120, 3), dtype=np.uint8)
    Image.fromarray(picture).save(trial)

    for f in (0.0, 0.5, 1.0):
        a = csharp(trial, tmp, flicker=f)
        b = python_side(trial, flicker=f)
        mx, mean, n, total = difference(a, b)
        ok = mx == 0
        faults += 0 if ok else 1
        print("   flicker filter %.2f : max %d, mean %.4f, %d of %d bytes different  %s"
              % (f, mx, mean, n, total, "EQUAL" if ok else "!! DIFFERS"))

    # --- B. the scaling ---------------------------------------------
    print("\nB. SCALING: NO equality is expected here.")
    print("   PIL works in fixed point and rounds per pass; C# works in double.\n")
    for name in ("photo.jpg", "test1.jpg", "test2.jpg"):
        path = os.path.join(HERE, "..", "..", "Firmware", "tools", name)
        if not os.path.exists(path):
            continue
        a = csharp(path, tmp)
        b = python_side(path)
        mx, mean, n, total = difference(a, b)
        print("   %-12s max %3d, mean %.3f, %.1f%% of the bytes different"
              % (name, mx, mean, 100.0 * n / total))

    for f in (tmp, tmp + ".error.txt", trial):
        if os.path.exists(f):
            os.remove(f)

    print()
    if faults:
        print("TRIAL A FAILED: the two implementations do not work out the same.")
        return 1
    print("Trial A passed: the arithmetic is identical in both implementations.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

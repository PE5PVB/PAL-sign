"""Demodulates a hacktv composite test signal back to 4:2:2 YCbCr."""
import numpy as np

FS = 13.5e6
FSC = 4433618.75
D = 2 * np.pi * FSC / FS
V_BLANK, V_WHITE = 3072.0, 832.0
LINES, SPL = 625, 864
ACT_X, ACT_W = 132, 720
CHROMA_HZ = 1.3e6


def decode(path, frames=4):
    s = np.fromfile(path, dtype="<i2").astype(np.float64)
    F = (V_BLANK - s.reshape(-1, LINES, SPL)) / (V_BLANK - V_WHITE)
    luma = F.mean(0)                       # carrier cancels over the full PAL sequence
    chroma = F[0] - luma                   # exact, so no cross luma

    x = np.arange(SPL)
    bq = (chroma[:, 80:102] * np.exp(-1j * D * x[80:102])).mean(1)
    ramp = (D * SPL) % (2 * np.pi)
    r = np.arange(LINES)
    q = np.angle(bq * np.exp(1j * ramp * r))

    act = np.arange(23, 623)
    mid = np.angle(np.exp(1j * q[act]) + np.exp(1j * q[act + 1]))
    sw = np.zeros(LINES)
    sw[act] = np.sign(np.sin(q[act] - mid))          # PAL V switch per line

    p = np.zeros(LINES)
    p[act] = q[act] - sw[act] * (3 * np.pi / 4) - ramp * act
    Z = 2 * chroma * np.exp(-1j * (D * x[None, :] + p[:, None]))

    n = 21
    t = np.arange(n) - (n - 1) / 2
    h = np.sinc(2 * (CHROMA_HZ / FS) * t) * np.hanning(n)
    h /= h.sum()
    Zf = np.apply_along_axis(lambda v: np.convolve(v, h, "same"), 1, Z)

    Y = np.clip(np.round(16 + 219 * luma), 16, 235)
    CB = np.clip(np.round(128 + 256.4 * Zf.real), 16, 240)
    CR = np.clip(np.round(128 + 182.2 * Zf.imag * sw[:, None]), 16, 240)
    return [w[:, ACT_X:ACT_X + ACT_W] for w in (Y, CB, CR)]


def frame576(path):
    """Two fields joined together into a picture of 576 lines."""
    Y, CB, CR = decode(path)
    out = []
    for w in (Y, CB, CR):
        f = np.zeros((576, ACT_W))
        f[0::2] = w[23:311]
        f[1::2] = w[336:624]
        out.append(f.astype(np.uint8))
    return out

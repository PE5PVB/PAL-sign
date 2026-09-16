#!/usr/bin/env python3
"""Reference model of the Ham PCM (v3) line code (src/testcards/pcmaudio.cpp,
PC Software/PCMDecoder/RowDecoder.cs): the encoder, an errors-and-
erasures Reed-Solomon decoder, and the self test that proves them
against each other. Also writes src/pcm_v3_tables.h.

Row, 180 bits in the middle 696 of the 720 active pixels (12 px guards):
  8 run-in (10101010), 4 guard (0000), sync 0x2E,
  core codeword RS(13,9): header 0xC0|n, 8 core bytes, 4 parity,
  enhancement: 6 bytes, CRC-8.
Each 14 bit sample s (two's complement) is split into a core byte
(s >> 6) & 0xFF and a 6 bit enhancement s & 0x3F; core bytes and
enhancement bytes are XORed with a fixed keystream before parity and
CRC. A decoder that loses the enhancement plays the row at 8 bits; one
that loses the core codeword conceals the row.

Run:  python tools/pcm_v3_ref.py   (from Firmware/)
"""
import random

PRIM = 0x11D
NPAR = 4
CORE_BYTES = 8
ENH_BYTES = 6
HEADER_V3 = 0xC0
SYNC = 0x2E
SEED = 0xACE1

EXP = [0] * 512
LOG = [0] * 256
_x = 1
for _i in range(255):
    EXP[_i] = _x
    LOG[_x] = _i
    _x <<= 1
    if _x & 0x100:
        _x ^= PRIM
for _i in range(255, 512):
    EXP[_i] = EXP[_i - 255]


def gmul(a, b):
    if a == 0 or b == 0:
        return 0
    return EXP[LOG[a] + LOG[b]]


def ginv(a):
    return EXP[255 - LOG[a]]


def poly_mul(p, q):
    r = [0] * (len(p) + len(q) - 1)
    for i, a in enumerate(p):
        for j, b in enumerate(q):
            r[i + j] ^= gmul(a, b)
    return r


def gen_poly():
    """g(x) = (x - 1)(x - a)(x - a^2)(x - a^3), highest degree first."""
    g = [1]
    for i in range(NPAR):
        g = poly_mul(g, [1, EXP[i]])
    return g


G = gen_poly()


def rs_encode(data):
    """Systematic: the NPAR parity bytes for the data bytes."""
    rem = [0] * NPAR
    for d in data:
        fb = d ^ rem[0]
        rem = rem[1:] + [0]
        if fb:
            for j in range(NPAR):
                rem[j] ^= gmul(G[j + 1], fb)
    return rem


def rs_syndromes(cw):
    s = []
    for i in range(NPAR):
        acc = 0
        for c in cw:
            acc = gmul(acc, EXP[i]) ^ c
        s.append(acc)
    return s


def poly_eval(p, x):
    """p highest degree first."""
    y = 0
    for c in p:
        y = gmul(y, x) ^ c
    return y


def solve_magnitudes(S, positions, n):
    """Magnitudes Y_j at the given positions from S_i = sum_j Y_j X_j^i,
    X_j = a^(n-1-p): Gaussian elimination over GF(256) on the 4 x k
    system, all 4 equations must agree. None if inconsistent."""
    k = len(positions)
    X = [EXP[(n - 1 - p) % 255] for p in positions]
    rows = []
    for i in range(NPAR):
        rows.append([EXP[(LOG[x] * i) % 255] for x in X] + [S[i]])
    r = 0
    piv = []
    for c in range(k):
        pr = next((i for i in range(r, NPAR) if rows[i][c]), None)
        if pr is None:
            return None
        rows[r], rows[pr] = rows[pr], rows[r]
        inv = ginv(rows[r][c])
        rows[r] = [gmul(v, inv) for v in rows[r]]
        for i in range(NPAR):
            if i != r and rows[i][c]:
                f = rows[i][c]
                rows[i] = [v ^ gmul(f, w) for v, w in zip(rows[i], rows[r])]
        piv.append(c)
        r += 1
    for i in range(r, NPAR):
        if rows[i][k]:
            return None
    return [rows[i][k] for i in range(k)]


def rs_decode(cw, erasures=()):
    """Errors-and-erasures decoding of one codeword (list, modified in
    place); erasures are positions (0 = first byte) known to be bad.
    Small code, so by trial: the erasure positions plus every choice
    of t further positions, t up to (NPAR - e) / 2, solved for the
    magnitudes; the first choice all four syndromes agree with wins.
    Returns the number of bytes changed, or None if uncorrectable."""
    from itertools import combinations
    n = len(cw)
    S = rs_syndromes(cw)
    if not any(S):
        return 0
    e = len(erasures)
    if e > NPAR:
        return None
    others = [q for q in range(n) if q not in erasures]
    for t in range((NPAR - e) // 2 + 1):
        for extra in combinations(others, t):
            pos = list(erasures) + list(extra)
            Y = solve_magnitudes(S, pos, n)
            if Y is None:
                continue
            changed = 0
            for p, y in zip(pos, Y):
                if y:
                    cw[p] ^= y
                    changed += 1
            return changed
    return None


def keystream(nbytes, seed=SEED):
    """16 bit Fibonacci LFSR x^16+x^14+x^13+x^11+1, taps 0xB400, one bit a
    step, MSB first into each byte. Same as v2."""
    lfsr = seed
    out = []
    for _ in range(nbytes):
        k = 0
        for _ in range(8):
            bit = lfsr & 1
            lfsr = (lfsr >> 1) ^ (0xB400 if bit else 0)
            k = (k << 1) | bit
        out.append(k)
    return out


KEY = keystream(CORE_BYTES + ENH_BYTES)


def crc8(data):
    c = 0
    for d in data:
        c ^= d
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


TEXT_ALPHABET = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,-/:;!?'()+=&@#*%<>\"_[]{}|"
TEXT_CHARS = 10
TEXT_SLOTS = TEXT_CHARS * 6 // 2


def text_slots(text):
    """The 30 two-bit values (header bits 4..3) that carry the text:
    upper case, unknown characters a space, padded to TEXT_CHARS."""
    sym = []
    for c in (text.upper() + " " * TEXT_CHARS)[:TEXT_CHARS]:
        sym.append(TEXT_ALPHABET.index(c) if c in TEXT_ALPHABET else 0)
    bits = 0
    for v in sym:
        bits = (bits << 6) | v
    return [(bits >> (60 - 2 * (k + 1))) & 3 for k in range(TEXT_SLOTS)]


def text_from_slots(slots):
    bits = 0
    for v in slots:
        bits = (bits << 2) | v
    return "".join(TEXT_ALPHABET[(bits >> (60 - 6 * (i + 1))) & 63] for i in range(TEXT_CHARS))


def encode_row(pairs, header_extra=0):
    """pairs: up to 4 (l, r) of 14 bit two's complement ints; header_extra:
    the text channel bits 5..3 (marker << 5 | two bits << 3). Returns the
    20 bytes after the sync byte."""
    n = len(pairs) | header_extra
    s = []
    for l, r in pairs:
        s += [l & 0x3FFF, r & 0x3FFF]
    s += [0] * (8 - len(s))
    core = [(v >> 6) & 0xFF for v in s]
    enh_bits = 0
    for v in s:
        enh_bits = (enh_bits << 6) | (v & 0x3F)
    enh = [(enh_bits >> (8 * (ENH_BYTES - 1 - i))) & 0xFF for i in range(ENH_BYTES)]
    core = [c ^ k for c, k in zip(core, KEY[:CORE_BYTES])]
    enh = [e ^ k for e, k in zip(enh, KEY[CORE_BYTES:])]
    cw = [HEADER_V3 | n] + core
    cw += rs_encode(cw)
    return cw + enh + [crc8(enh)]


def decode_row(b, erasures=()):
    """Returns (pairs, quality) with quality 'full', 'coarse' or None."""
    cw = list(b[:13])
    if rs_decode(cw, erasures) is None:
        return None, None
    if (cw[0] & 0xC0) != HEADER_V3 or (cw[0] & 7) > 4:
        return None, None
    n = cw[0] & 7
    core = [c ^ k for c, k in zip(cw[1:9], KEY[:CORE_BYTES])]
    enh = list(b[13:19])
    full = crc8(enh) == b[19]
    s = [c << 6 for c in core]
    if full:
        enh = [e ^ k for e, k in zip(enh, KEY[CORE_BYTES:])]
        bits = 0
        for e in enh:
            bits = (bits << 8) | e
        for i in range(8):
            s[i] |= (bits >> (6 * (7 - i))) & 0x3F
    s = [v - 0x4000 if v & 0x2000 else v for v in s]
    return [(s[2 * i], s[2 * i + 1]) for i in range(n)], ('full' if full else 'coarse')


def bits_per_row():
    return 8 + 4 + 8 + 8 * (13 + ENH_BYTES + 1)


# The two 32 kHz qualities of the card: LQ (12 bit samples: core byte
# bits 11..4, a 4 bit enhancement each, 3 enhancement bytes + CRC-8,
# 120 bits after the sync in cells of 696/140 px) and Voice (8 bit
# G.711 A-law in the core bytes alone, 88 bits, cells of 696/108 px).
# Both share the RS(11,7) core codeword, the header layout (marker
# 0x80 LQ, 0x40 Voice, text bits, n = 0..3) and the keystream.
LOW_PAIRS = 3
HEADER_LQ = 0x80
HEADER_VOICE = 0x40
HEADER_NARROW = 0x00
NARROW_SAMPLES = 2


def alaw_encode(pcm):
    """ITU-T G.711 A-law of a 16 bit sample, even bits inverted."""
    v = pcm >> 3
    mask = 0xD5
    if v < 0:
        mask = 0x55
        v = -v - 1
    seg = 0
    while seg < 8 and v > ((0x20 << seg) - 1):
        seg += 1
    if seg >= 8:
        return 0x7F ^ mask
    aval = seg << 4
    aval |= ((v >> 1) & 0xF) if seg < 2 else ((v >> seg) & 0xF)
    return aval ^ mask


def alaw_decode(a):
    a ^= 0x55
    t = (a & 0xF) << 4
    seg = (a & 0x70) >> 4
    if seg == 0:
        t += 8
    elif seg == 1:
        t += 0x108
    else:
        t += 0x108
        t <<= seg - 1
    return t if (a & 0x80) else -t


def low_encode_row(pairs, voice, header_extra=0):
    """pairs: up to 3 (l, r) of 16 bit ints. Returns the bytes after the
    sync byte: 11 (Voice) or 15 (LQ)."""
    n = len(pairs)
    s = []
    for l, r in pairs:
        s += [l, r]
    s += [0] * (6 - len(s))
    if voice:
        core = [alaw_encode(v) ^ k for v, k in zip(s, KEY[:6])]
        cw = [HEADER_VOICE | n | header_extra] + core
        return cw + rs_encode(cw)
    w = [(v >> 4) & 0xFFF for v in s]
    core = [((v >> 4) & 0xFF) ^ k for v, k in zip(w, KEY[:6])]
    enh = [(((w[2 * i] & 0xF) << 4) | (w[2 * i + 1] & 0xF)) ^ KEY[8 + i] for i in range(3)]
    cw = [HEADER_LQ | n | header_extra] + core
    return cw + rs_encode(cw) + enh + [crc8(enh)]


def narrow_encode_row(samples, header_extra=0):
    """Voice narrow: up to 2 mono 16 bit samples, A-law, RS(7,3);
    returns the 7 bytes after the sync byte."""
    n = len(samples)
    s = list(samples) + [0] * (NARROW_SAMPLES - n)
    cw = [HEADER_NARROW | n | header_extra] + [alaw_encode(v) ^ k for v, k in zip(s, KEY[:2])]
    return cw + rs_encode(cw)


def narrow_decode_row(b, erasures=()):
    cw = list(b[:7])
    if rs_decode(cw, erasures) is None:
        return None, None
    if (cw[0] & 0xC0) != HEADER_NARROW or (cw[0] & 7) > NARROW_SAMPLES:
        return None, None
    n = cw[0] & 7
    return [alaw_decode(c ^ k) for c, k in zip(cw[1:3], KEY[:2])][:n], 'full'


def low_decode_row(b, voice, erasures=()):
    """Returns (pairs as 16 bit values, quality)."""
    cw = list(b[:11])
    if rs_decode(cw, erasures) is None:
        return None, None
    if (cw[0] & 0xC0) != (HEADER_VOICE if voice else HEADER_LQ) or (cw[0] & 7) > LOW_PAIRS:
        return None, None
    n = cw[0] & 7
    core = [c ^ k for c, k in zip(cw[1:7], KEY[:6])]
    if voice:
        s = [alaw_decode(c) for c in core]
        return [(s[2 * i], s[2 * i + 1]) for i in range(n)], 'full'
    enh = list(b[11:14])
    full = crc8(enh) == b[14]
    s = [c << 4 for c in core]
    if full:
        e = [v ^ k for v, k in zip(enh, KEY[8:11])]
        for i in range(6):
            s[i] |= (e[i // 2] >> 4) & 0xF if i % 2 == 0 else e[i // 2] & 0xF
    s = [(v - 0x1000 if v & 0x800 else v) << 4 for v in s]
    return [(s[2 * i], s[2 * i + 1]) for i in range(n)], ('full' if full else 'coarse')


def main():
    rnd = random.Random(1)
    assert bits_per_row() == 180, bits_per_row()
    ok = 0
    crc_miss = 0
    for trial in range(20000):
        n = rnd.randint(0, 4)
        pairs = [(rnd.randint(-8192, 8191), rnd.randint(-8192, 8191)) for _ in range(n)]
        row = encode_row(pairs, rnd.randint(0, 7) << 3)
        # 0, 1 or 2 byte errors anywhere; or 3-4 erasures (some clean).
        kind = trial % 5
        r = row[:]
        erasures = ()
        if kind in (1, 2):
            for p in rnd.sample(range(20), kind):
                r[p] ^= rnd.randint(1, 255)
        elif kind == 3:
            pos = rnd.sample(range(13), 4)
            erasures = tuple(pos)
            for p in pos[:rnd.randint(0, 4)]:
                r[p] ^= rnd.randint(1, 255)
        elif kind == 4:
            pos = rnd.sample(range(13), 2)
            erasures = tuple(pos)
            for p in pos:
                r[p] ^= rnd.randint(1, 255)
            r[rnd.choice([q for q in range(13) if q not in pos])] ^= rnd.randint(1, 255)
        got, q = decode_row(r, erasures)
        core_hit = any(r[i] != row[i] for i in range(13))
        enh_hit = any(r[i] != row[i] for i in range(13, 20))
        if got is None:
            raise SystemExit("trial %d kind %d: uncorrectable" % (trial, kind))
        if enh_hit:
            if q == 'full':
                crc_miss += 1   # CRC-8 passes a damaged enhancement 1 in 256
                continue
            assert q == 'coarse', (trial, q)
            assert all(abs(a - b) < 64 and abs(c - d) < 64 for (a, c), (b, d) in zip(got, pairs)), trial
        else:
            assert q == 'full' and got == pairs, (trial, q, got, pairs)
        ok += 1
    # Three random errors in the codeword must not decode silently
    # into a wrong header often; count the miscorrections.
    mis = 0
    for trial in range(20000):
        pairs = [(rnd.randint(-8192, 8191), rnd.randint(-8192, 8191)) for _ in range(4)]
        row = encode_row(pairs, rnd.randint(0, 7) << 3)
        r = row[:]
        for p in rnd.sample(range(13), 3):
            r[p] ^= rnd.randint(1, 255)
        got, q = decode_row(r)
        if got is not None and got != pairs:
            mis += 1
    print("RS(13,9) errors/erasures + scrambler + CRC-8 reference: OK, %d rows, %d damaged enhancements passed the CRC" % (ok, crc_miss))
    print("3 errors in the core codeword: %d of 20000 miscorrected (rest detected)" % mis)
    print("bits per row: %d of 180" % bits_per_row())
    assert text_from_slots(text_slots("pe5pvb jo21")) == "PE5PVB JO2"
    assert len(TEXT_ALPHABET) == 64

    # LQ and Voice: the same trials on the shorter rows; A-law must
    # round-trip within its own step.
    for voice in (False, True):
        ok = 0
        for trial in range(10000):
            n = rnd.randint(0, LOW_PAIRS)
            pairs = [(rnd.randint(-32768, 32767), rnd.randint(-32768, 32767)) for _ in range(n)]
            row = low_encode_row(pairs, voice, rnd.randint(0, 7) << 3)
            r = row[:]
            kind = trial % 3
            if kind in (1, 2):
                for p in rnd.sample(range(len(row)), kind):
                    r[p] ^= rnd.randint(1, 255)
            got, q = low_decode_row(r, voice)
            assert got is not None, (voice, trial, kind)
            crc_miss = 0
            for (gl, gr), (l, rr) in zip(got, pairs):
                if voice:
                    # A-law: within its own step at that level.
                    assert abs(gl - l) <= abs(l) // 16 + 16 and abs(gr - rr) <= abs(rr) // 16 + 16, (trial, gl, l)
                elif q == 'coarse':
                    # Core only: within the 4 enhancement bits (16 x 16).
                    assert abs(gl - l) < 256 and abs(gr - rr) < 256, (trial, q, gl, l)
                elif abs(gl - l) > 16 or abs(gr - rr) > 16:
                    # Only a damaged enhancement the CRC-8 let through
                    # (1 in 256) explains a full row this far off.
                    assert abs(gl - l) < 256 and abs(gr - rr) < 256, (trial, q, gl, l)
                    crc_miss += 1
            ok += 1
        print("%s reference: OK, %d rows" % ("Voice A-law" if voice else "LQ 12 bit", ok))
    ok = 0
    for trial in range(10000):
        n = rnd.randint(0, NARROW_SAMPLES)
        samples = [rnd.randint(-32768, 32767) for _ in range(n)]
        row = narrow_encode_row(samples, rnd.randint(0, 7) << 3)
        r = row[:]
        kind = trial % 3
        if kind in (1, 2):
            for p in rnd.sample(range(7), kind):
                r[p] ^= rnd.randint(1, 255)
        got, q = narrow_decode_row(r)
        assert got is not None, (trial, kind)
        for g, v in zip(got, samples):
            assert abs(g - v) <= abs(v) // 16 + 16, (trial, g, v)
        ok += 1
    print("Voice narrow reference: OK, %d rows" % ok)

    step = [0] * 256
    for b in range(256):
        v = 0
        for j in range(NPAR):
            v = (v << 8) | gmul(G[j + 1], b)
        step[b] = v
    tab = [crc8(bytes([b])) for b in range(256)]
    with open("src/pcm_v3_tables.h", "w", newline="\n") as f:
        f.write("// Generated by tools/pcm_v3_ref.py; do not edit by hand.\n")
        f.write("// Ham PCM (v3) RS(13,9) over GF(256), poly 0x11D, g(x) = (x-1)(x-a)(x-a^2)(x-a^3).\n")
        f.write("#ifndef PCM_V3_TABLES_H\n#define PCM_V3_TABLES_H\n\n#include <stdint.h>\n\n")
        f.write("// Parity feedback per byte: g1 fb << 24 | g2 fb << 16 | g3 fb << 8 | g4 fb.\n")
        f.write("static const uint32_t RS_STEP[256] = {\n")
        for i in range(0, 256, 8):
            f.write("  " + ", ".join("0x%08X" % v for v in step[i:i + 8]) + ",\n")
        f.write("};\n// CRC-8, poly 0x07, one byte a step.\n")
        f.write("static const uint8_t CRC8_TABLE[256] = {\n")
        for i in range(0, 256, 16):
            f.write("  " + ", ".join(str(v) for v in tab[i:i + 16]) + ",\n")
        f.write("};\n// The keystream, 8 core bytes then 6 enhancement bytes.\n")
        f.write("static const uint8_t SCRAMBLE_KEY[14] = {" + ", ".join("0x%02X" % k for k in KEY) + "};\n")
        f.write("\n#endif  // PCM_V3_TABLES_H\n")
    print("wrote src/pcm_v3_tables.h")

    # Vectors for PcmDecoder --selftest: 20 row bytes as hex, then this
    # model's verdict ('-' for a lost row, else full:/coarse: and the
    # pairs), 600 rows with 0-2 byte errors anywhere or 3 in the core.
    rnd = random.Random(3)
    out = []
    for t in range(600):
        n = rnd.randint(0, 4)
        pairs = [(rnd.randint(-8192, 8191), rnd.randint(-8192, 8191)) for _ in range(n)]
        d = encode_row(pairs, rnd.randint(0, 7) << 3)
        kind = t % 4
        if kind in (1, 2):
            for p in rnd.sample(range(20), kind):
                d[p] ^= rnd.randint(1, 255)
        elif kind == 3:
            for p in rnd.sample(range(13), 3):
                d[p] ^= rnd.randint(1, 255)
        got, q = decode_row(d)
        verdict = '-' if got is None else q + ':' + ','.join('%d/%d' % pq for pq in got)
        out.append(''.join('%02X' % b for b in d) + ' ' + verdict)
    path = "../PC Software/PCMDecoder/selftest_vectors.txt"
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("wrote %s" % path)

    # And the text channel: one full cycle of 30 clean rows for a known
    # text, first line the text PcmDecoder must assemble from them.
    text = "PE5PVB JO2"
    slots = text_slots(text)
    rows = []
    for k in range(TEXT_SLOTS):
        pairs = [(rnd.randint(-8192, 8191), rnd.randint(-8192, 8191)) for _ in range(3)]
        rows.append(''.join('%02X' % b for b in encode_row(pairs, ((1 if k == 0 else 0) << 5) | (slots[k] << 3))))
    # LQ and Voice vectors: the row bytes as hex, then the verdict with
    # the 16 bit values PcmDecoder puts out.
    for voice, name in ((False, "selftest_lq.txt"), (True, "selftest_voice.txt")):
        out = []
        for t in range(300):
            n = rnd.randint(0, LOW_PAIRS)
            pairs = [(rnd.randint(-32768, 32767), rnd.randint(-32768, 32767)) for _ in range(n)]
            d = low_encode_row(pairs, voice, rnd.randint(0, 7) << 3)
            kind = t % 4
            if kind in (1, 2):
                for p in rnd.sample(range(len(d)), kind):
                    d[p] ^= rnd.randint(1, 255)
            elif kind == 3:
                for p in rnd.sample(range(11), 3):
                    d[p] ^= rnd.randint(1, 255)
            got, q = low_decode_row(d, voice)
            verdict = '-' if got is None else q + ':' + ','.join('%d/%d' % pq for pq in got)
            out.append(''.join('%02X' % v for v in d) + ' ' + verdict)
        path = "../PC Software/PCMDecoder/" + name
        with open(path, "w", newline=chr(10)) as f:
            f.write(chr(10).join(out) + chr(10))
        print("wrote %s" % path)

    out = []
    for t in range(300):
        n = rnd.randint(0, NARROW_SAMPLES)
        samples = [rnd.randint(-32768, 32767) for _ in range(n)]
        d = narrow_encode_row(samples, rnd.randint(0, 7) << 3)
        kind = t % 4
        if kind in (1, 2):
            for p in rnd.sample(range(7), kind):
                d[p] ^= rnd.randint(1, 255)
        elif kind == 3:
            for p in rnd.sample(range(7), 3):
                d[p] ^= rnd.randint(1, 255)
        got, q = narrow_decode_row(d)
        verdict = '-' if got is None else q + ':' + ','.join('%d/%d' % (v, v) for v in got)
        out.append(''.join('%02X' % v for v in d) + ' ' + verdict)
    path = "../PC Software/PCMDecoder/selftest_narrow.txt"
    with open(path, "w", newline=chr(10)) as f:
        f.write(chr(10).join(out) + chr(10))
    print("wrote %s" % path)

    path = "../PC Software/PCMDecoder/selftest_text.txt"
    with open(path, "w", newline=chr(10)) as f:
        f.write(text + chr(10) + chr(10).join(rows) + chr(10))
    print("wrote %s" % path)


if __name__ == "__main__":
    main()

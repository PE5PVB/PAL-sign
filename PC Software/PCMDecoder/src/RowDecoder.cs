// Reads back what Firmware/src/testcards/pcmaudio.cpp (the Ham PCM
// card) writes into each active row: 4 pixels a bit, an 8 bit
// alternating run-in, 4 guard bits, the sync byte, then the RS(13,9)
// core codeword (header 0xC0|n, 8 core bytes, 4 parity), 6 enhancement
// bytes and their CRC-8. These constants have to stay in step with that
// file by hand; the reference for both is Firmware/tools/pcm_v3_ref.py.
//
// Robustness, in this order: the core codeword is decoded with up to
// two byte errors, or up to four erasures taken from the bytes whose
// bits sat closest to the slicing threshold; a damaged enhancement
// (CRC) leaves the row playing at 8 bits; a lost core codeword leaves
// a hole that is filled by interpolating to the next good row, so a
// hole costs one row of latency and nothing else.
using System;
using System.Collections.Generic;

namespace PcmDecoder;

public readonly struct SamplePair
{
    public readonly short Left;
    public readonly short Right;
    public SamplePair(short l, short r) { Left = l; Right = r; }
}

// What became of the last row handed to DecodeRow(), for a per-row
// picture of the signal.
public enum RowStatus : byte { None, Ok, Coarse, Concealed, NoRunIn, NoSync }

public sealed class RowDecoder
{
    public RowStatus LastStatus;
    private const byte SyncByte = 0x2E;
    private const int MaxPairs = 4;   // MAX_PAIRS in pcmaudio.cpp
    private const int PreambleBits = 8;
    // GUARD_BITS in pcmaudio.cpp: always-low bits between the run-in
    // and the sync byte, a quiet stretch for the video filter to settle
    // out of the run-in's toggling before the framing byte.
    private const int GuardBits = 4;
    private const int CoreWord = 13;   // header + 8 core + 4 parity
    private const int EnhBytes = 6;
    private const int RowBytes = CoreWord + EnhBytes + 1;
    private const byte HeaderV3 = 0xC0;
    // The three qualities of the Ham PCM card, told apart by the bit
    // period the preamble locks to, before anything is read:
    //   0 HQ    48 kHz, 14 bit, 160 bits after the sync at 4 px/bit
    //   1 LQ    32 kHz, 12 bit, RS(11,7) + 3 enhancement bytes + CRC,
    //           120 bits at 696/140 px/bit
    //   2 Voice 32 kHz, 8 bit A-law in the core bytes alone, 88 bits at
    //           696/108 px/bit
    //   3 Voice narrow: 16 kHz mono, 2 A-law samples in a RS(7,3)
    //           codeword, 56 bits at 696/76 px/bit
    // The data sits in the middle 696 of the 720 active pixels: 12
    // guard pixels either side keep a shifted capture window (this
    // chain's Blackmagic clips ~7.5 px off the line end) from eating
    // the row's tail.
    private static readonly double[] Periods = { 696.0 / 180.0, 696.0 / 140.0, 696.0 / 108.0, 696.0 / 76.0 };
    private static readonly int[] RowBitsOf = { RowBytes * 8, 120, 88, 56 };
    private static readonly int[] CoreWordOf = { CoreWord, 11, 11, 7 };
    private static readonly int[] EnhBytesOf = { EnhBytes, 3, 0, 0 };
    private static readonly byte[] MarkerOf = { 0xC0, 0x80, 0x40, 0x00 };
    private static readonly int[] MaxPairsOf = { MaxPairs, 3, 3, 2 };
    public int LastFamily { get; private set; }
    public int LastSampleRate => LastFamily == 0 ? 48000 : LastFamily == 3 ? 16000 : 32000;
    private static int FamilyOf(double period)
    {
        int best = 0;
        for (int f = 1; f < Periods.Length; f++)
            if (Math.Abs(period - Periods[f]) < Math.Abs(period - Periods[best])) best = f;
        return best;
    }
    // On a coarse row: which of the tail bytes (0 = first enhancement
    // byte .. 6 = CRC for Ham PCM) had the smallest bit margin, and
    // that margin; a diagnostic for where the tail goes wrong.
    public int LastWeakByte { get; private set; } = -1;
    public int LastWeakMargin { get; private set; }
    // The text channel in the header's bits 5..3: bit 5 marks the first
    // of TextSlots rows, bits 4..3 carry two bits each, TextChars
    // characters of 6 bits from TextAlphabet, MSB first.
    private const int TextChars = 10;
    private const int TextSlots = TextChars * 6 / 2;
    private const string TextAlphabet = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,-/:;!?'()+=&@#*%<>\"_[]{}|";
    public string Text { get; private set; } = "";
    private readonly int[] _textSlots = new int[TextSlots];
    private int _textCount = -1;     // -1: waiting for a marker
    // Rows the hole filler waits for a good row before giving up and
    // holding the last pair instead (a fade, not a sparkle).
    private const int MaxHoleRows = 8;
    private const int HolePairs = 3;   // pairs assumed for a row whose header is lost (48000 / 15625)

    // How far into the row to search for the preamble before giving up.
    // Measured on the board (Blackmagic Intensity Pro over composite):
    // the active picture this tool receives starts a handful of pixels
    // later than the firmware's own pixel 0, around 6 on one row --
    // not necessarily the same on every capture setup, hence a search.
    // The run-in nominally starts at pixel 12 (the left guard) and a
    // capture window can shift it either way; 36 covers 12 + 8 with
    // margin, and a wrong start still fails on its first bits.
    private const int MaxSearchPixels = 36;

    // The two bit periods that exist: 4 pixels (Ham PCM, PX_PER_BIT in
    // pcmaudio.cpp) and 720/152 (Ham PCM LQ). Composite video carries no
    // explicit pixel clock: the capture card regenerates its own sample
    // clock from sync alone, so each is searched over a small range.
    private const double PeriodRange = 0.16;
    private const double PeriodStep = 0.02;
    // Score lost per pixel of period away from the nominal one: a period
    // 0.02 px off reads a clean preamble too, but is 3 px out by the end
    // of the row, and on real rows it won on noise often enough to cost
    // the enhancement (coarse) or the core (concealed) on ~3% of the
    // rows over a cable. With this only a clearly better margin can
    // move the period off nominal.
    private const double PeriodPenalty = 2500;
    private const double PeriodMin = 3.2;
    private const double PeriodMax = 10.0;   // Voice narrow sits at 9.16 px a bit

    // Diagnostics, cumulative.
    public long RowsOk;              // core codeword decoded (full or coarse)
    public long RowsRunInMismatch;
    public long RowsSyncMismatch;
    public long RowsCorrected;       // core codeword needed a correction
    public long RowsErasureFixed;    // ... and only the erasure retry managed it
    public long RowsCoarse;          // enhancement lost, played at 8 bits
    public long RowsConcealed;       // core codeword lost, interpolated or held
    // Purely informational, from the last successful lock.
    public int LastLockPixel = -1;
    public double LastLockPeriod = 0;
    public int LastSyncShift = 0;

    private SamplePair _lastPair;
    private int _holeRows;           // rows lost since the last good one, awaiting a fill

    internal static int YAtPixel(byte[] pixels, int rowOffset, int p)
    {
        int quad = p / 2;
        int off = rowOffset + quad * 4 + (p % 2 == 0 ? 1 : 3);
        return pixels[off];
    }

    private readonly struct Candidate
    {
        public readonly int P0, Threshold;
        public readonly double Period, SyncStart, Score;
        public Candidate(int p0, double period, int threshold, double syncStart, double score)
        { P0 = p0; Period = period; Threshold = threshold; SyncStart = syncStart; Score = score; }
    }

    // The row after the sync byte sliced at a candidate's period and
    // threshold into bytes, MSB first (the LQ row's last byte holds its
    // final 4 enhancement bits above a zero nibble), with per byte the
    // smallest distance any of its bits had from the threshold: the
    // erasure candidates.
    private static bool Slice(byte[] pixels, int rowOffset, int width, Candidate c, int family,
        out byte[] bytes, out int[] margin, out int[] bitMargin)
    {
        int rowBits = RowBitsOf[family];
        int nBytes = (rowBits + 7) / 8;
        bytes = new byte[nBytes + 1];
        margin = new int[nBytes + 1];
        bitMargin = new int[rowBits];
        if (c.SyncStart + (8 + rowBits) * c.Period > width + 4) return false;
        Span<int> bits = stackalloc int[rowBits];
        // Sampled mid-cell from the sync byte's start, with the phase
        // following the bit transitions: at every change between two
        // neighbouring bits the threshold crossing between those two
        // sample pixels is located, and the phase nudged by half of how
        // far it sits from the cell boundary the current phase predicts.
        // The capture card's period is 4.00 on average but not on every
        // row (the top rows after vertical sync sit 0.01-0.02 px off),
        // and over 180 bits that is the difference between the row's
        // tail reading and not. Modelled offline: rows at 3.94..4.00 px
        // all read; above 4.00 only a last bit that falls past pixel 719
        // is lost, which is the picture's edge, not the tracker.
        SliceAt(pixels, rowOffset, width, c.SyncStart, c.Period, c.Threshold, bits, margin, bitMargin);
        for (int i = 0; i < rowBits; i++) bytes[i / 8] |= (byte)(bits[i] << (7 - i % 8));
        return true;
    }

    // The row bits after the sync byte at one period, sampled mid-cell
    // from the sync byte's start, with per byte the smallest distance
    // any bit kept from the threshold.
    private static void SliceAt(byte[] pixels, int rowOffset, int width, double syncStart, double period,
        int threshold, Span<int> bits, int[] margin, int[] bitMargin)
    {
        for (int i = 0; i < margin.Length; i++) margin[i] = int.MaxValue;
        double next = syncStart + 8 * period;
        int prevPx = -1, prevBit = -1;
        for (int i = 0; i < bits.Length; i++)
        {
            // The last cell may end a few pixels past the row; reading
            // past it would index the next row.
            int px = (int)Math.Round(next);
            if (px >= width) px = width - 1;
            int y = YAtPixel(pixels, rowOffset, px);
            int bit = y > threshold ? 1 : 0;
            bits[i] = bit;
            int d = Math.Abs(y - threshold);
            if (d < margin[i / 8]) margin[i / 8] = d;
            bitMargin[i] = d;
            if (prevPx >= 0 && bit != prevBit)
            {
                // The one crossing between the previous sample pixel and
                // this one (searching further back caught the boundary
                // before that and threw the phase off).
                int last = YAtPixel(pixels, rowOffset, prevPx) > threshold ? 1 : 0;
                for (int x = prevPx + 1; x <= px; x++)
                {
                    if ((YAtPixel(pixels, rowOffset, x) > threshold ? 1 : 0) != last)
                    {
                        double err = (next - period / 2) - (x - 0.5);   // > 0: signal early
                        if (err > 1.5) err = 1.5; else if (err < -1.5) err = -1.5;
                        next -= err * 0.5;
                        break;
                    }
                }
            }
            prevPx = px;
            prevBit = bit;
            next += period;
        }
    }

    public SamplePair[] DecodeRow(byte[] pixels, int rowOffset, int rowBytes)
    {
        int width = rowBytes / 2;   // 2 bytes/pixel, packed 4:2:2

        var candidates = FindPreamble(pixels, rowOffset, width);
        if (candidates.Count == 0)
        {
            RowsRunInMismatch++;
            LastStatus = RowStatus.NoRunIn;
            return Array.Empty<SamplePair>();
        }

        // The preamble alone cannot always tell the two bit periods
        // apart (a 4.74 px row also reads a clean preamble at 4.3 px:
        // the run-in is symmetric and the guard bits hide a skipped
        // cell), so every candidate that passed it is tried in score
        // order and the first whose core codeword decodes, with a header
        // of its own format, is the row. The erasure retry only runs on
        // the best candidate once none decodes cleanly.
        byte[]? bytes = null;
        int[]? margin = null;
        int[]? bitMargin = null;
        byte[]? cw = null;
        int status = -1;
        int family = 0;
        Candidate chosen = candidates[0];
        foreach (var c in candidates)
        {
            int cf = FamilyOf(c.Period);
            if (!Slice(pixels, rowOffset, width, c, cf, out var by, out var mg, out var bm)) continue;
            int cwLen = CoreWordOf[cf];
            var word = new byte[cwLen];
            Array.Copy(by, word, cwLen);
            int st = Rs.Decode(word, Array.Empty<int>());
            if (st >= 0 && (word[0] & 0xC0) == MarkerOf[cf])
            {
                bytes = by; margin = mg; bitMargin = bm; cw = word; status = st; family = cf; chosen = c;
                break;
            }
            if (bytes == null) { bytes = by; margin = mg; bitMargin = bm; family = cf; chosen = c; }
        }
        LastLockPixel = chosen.P0;
        LastLockPeriod = chosen.Period;
        LastSyncShift = 0;
        LastFamily = family;
        if (bytes == null || margin == null || bitMargin == null) { LastStatus = RowStatus.Concealed; return Lost(); }
        int coreWord2 = CoreWordOf[family];
        if (cw == null)
        {
            // Errors-only failed everywhere: retry the best candidate with
            // the doubtful bytes declared erased, the two weakest first
            // (one further error allowed), then the four weakest. Only
            // bytes whose margin is clearly below the row's typical one
            // count as doubtful: four erasures always "solve", so without
            // that filter a row with three errors would decode into
            // nonsense.
            cw = new byte[coreWord2];
            int[] order = new int[coreWord2];
            for (int i = 0; i < coreWord2; i++) order[i] = i;
            Array.Sort(order, (x, z) => margin[x].CompareTo(margin[z]));
            int doubtful = 0;
            int typical = margin[order[coreWord2 / 2]];
            while (doubtful < 4 && margin[order[doubtful]] * 2 < typical) doubtful++;
            foreach (int count in new[] { 2, 4 })
            {
                if (count > doubtful) break;
                Array.Copy(bytes, cw, coreWord2);
                status = Rs.Decode(cw, order[..count]);
                if (status >= 0) { RowsErasureFixed++; break; }
            }
        }
        byte marker = MarkerOf[family];
        int maxPairs = MaxPairsOf[family];
        if (status < 0 || (cw[0] & 0xC0) != marker || (cw[0] & 7) > maxPairs)
        {
            RowsConcealed++;
            LastStatus = RowStatus.Concealed;
            return Lost();
        }
        if (status > 0) RowsCorrected++;
        int n = cw[0] & 7;
        TextChannel(cw[0]);

        int samples = family == 0 ? 8 : family == 3 ? 2 : 6;
        int enhBytes = EnhBytesOf[family];
        bool full = enhBytes == 0 || Crc8(bytes, coreWord2, enhBytes) == bytes[coreWord2 + enhBytes];
        if (!full) full = RepairTail(bytes, bitMargin, coreWord2, enhBytes);
        if (!full)
        {
            RowsCoarse++;
            LastWeakByte = -1; LastWeakMargin = int.MaxValue;
            for (int i = coreWord2; i <= coreWord2 + enhBytes; i++)
                if (margin[i] < LastWeakMargin) { LastWeakMargin = margin[i]; LastWeakByte = i - coreWord2; }
        }
        LastStatus = full ? RowStatus.Ok : RowStatus.Coarse;
        // The samples as 16 bit values.
        var s = new int[8];
        for (int i = 0; i < samples; i++)
        {
            int core = cw[1 + i] ^ ScrambleKey[i];
            if (family >= 2) { s[i] = AlawDecode((byte)core); continue; }
            int v = family == 0 ? core << 6 : core << 4;
            if (full)
            {
                if (family == 0)
                {
                    ulong acc = 0;
                    for (int k = 0; k < enhBytes; k++) acc = (acc << 8) | (byte)(bytes[coreWord2 + k] ^ ScrambleKey[8 + k]);
                    v |= (int)(acc >> (6 * (7 - i))) & 0x3F;
                }
                else
                {
                    int e = bytes[coreWord2 + i / 2] ^ ScrambleKey[8 + i / 2];
                    v |= (i % 2 == 0) ? (e >> 4) & 0xF : e & 0xF;
                }
            }
            if (family == 0) { if ((v & 0x2000) != 0) v -= 0x4000; v <<= 2; }
            else { if ((v & 0x800) != 0) v -= 0x1000; v <<= 4; }
            s[i] = v;
        }
        var res = new SamplePair[n];
        if (family == 3)
        {
            // Mono: the one sample on both sides.
            for (int i = 0; i < n; i++) res[i] = new SamplePair((short)s[i], (short)s[i]);
        }
        else
        {
            for (int i = 0; i < n; i++) res[i] = new SamplePair((short)s[2 * i], (short)s[2 * i + 1]);
        }
        RowsOk++;

        var outp = res;
        if (_holeRows > 0 && n > 0)
        {
            // Fill the hole by a straight line from the last good pair to
            // this row's first, HolePairs a lost row.
            int hole = _holeRows * HolePairs;
            outp = new SamplePair[hole + n];
            for (int i = 0; i < hole; i++)
            {
                double f = (i + 1) / (double)(hole + 1);
                outp[i] = new SamplePair(
                    (short)Math.Round(_lastPair.Left + (res[0].Left - _lastPair.Left) * f),
                    (short)Math.Round(_lastPair.Right + (res[0].Right - _lastPair.Right) * f));
            }
            Array.Copy(res, 0, outp, hole, n);
            _holeRows = 0;
        }
        if (n > 0) _lastPair = res[n - 1];
        return outp;
    }

    // The tail's doubtful bits tried both ways: the last pixels of the
    // row arrive on the threshold from a capture card (measured: on every
    // enhancement failure over a cable the CRC byte, pixels 688-719, was
    // the weakest with a margin of 2 in 110), so up to TailTrials bits
    // whose margin is below TailDoubt are flipped in every combination
    // and the row is taken when exactly one of them satisfies the CRC.
    private const int TailDoubt = 12;
    private const int TailTrials = 4;
    public long RowsTailRepaired;

    private bool RepairTail(byte[] bytes, int[] bitMargin, int coreWord, int enhBytes)
    {
        // Bit index in the row of tail bit j: byte coreWord + j / 8, bit 7 - j % 8.
        int tailBits = (enhBytes + 1) * 8;
        var doubtful = new List<int>();
        for (int j = 0; j < tailBits; j++)
        {
            if (bitMargin[coreWord * 8 + j] < TailDoubt) doubtful.Add(j);
        }
        if (doubtful.Count == 0 || doubtful.Count > TailTrials) return false;
        var work = new byte[bytes.Length];
        int hits = 0;
        byte[]? good = null;
        for (int mask = 1; mask < (1 << doubtful.Count); mask++)
        {
            Array.Copy(bytes, work, bytes.Length);
            for (int b = 0; b < doubtful.Count; b++)
            {
                if ((mask & (1 << b)) == 0) continue;
                int j = doubtful[b];
                work[coreWord + j / 8] ^= (byte)(1 << (7 - j % 8));
            }
            if (Crc8(work, coreWord, enhBytes) == work[coreWord + enhBytes])
            {
                hits++;
                good = (byte[])work.Clone();
            }
        }
        if (hits != 1 || good == null) return false;
        Array.Copy(good, bytes, bytes.Length);
        RowsTailRepaired++;
        return true;
    }

    // Collects the header's text bits; a lost row in a cycle drops
    // that cycle, the next marker starts afresh (2 ms later).
    private void TextChannel(int header)
    {
        if ((header & 0x20) != 0) _textCount = 0;
        if (_textCount < 0) return;
        _textSlots[_textCount++] = (header >> 3) & 3;
        if (_textCount < TextSlots) return;
        _textCount = -1;
        ulong bits = 0;
        foreach (int v in _textSlots) bits = (bits << 2) | (uint)v;
        var chars = new char[TextChars];
        for (int i = 0; i < TextChars; i++) chars[i] = TextAlphabet[(int)(bits >> (60 - 6 * (i + 1))) & 63];
        Text = new string(chars).TrimEnd();
    }

    // A row without a usable core codeword: remembered for the next
    // good row to interpolate over; after MaxHoleRows the last pair is
    // held instead so a long fade does not pile up latency.
    private SamplePair[] Lost()
    {
        _textCount = -1;
        _holeRows++;
        if (_holeRows <= MaxHoleRows) return Array.Empty<SamplePair>();
        _holeRows = MaxHoleRows;   // keep waiting, but emit as we go
        var held = new SamplePair[HolePairs];
        for (int i = 0; i < HolePairs; i++) held[i] = _lastPair;
        return held;
    }

    private static readonly byte[] ScrambleKey = BuildScrambleKey();
    private static byte[] BuildScrambleKey()
    {
        var key = new byte[14];
        int lfsr = 0xACE1;
        for (int i = 0; i < 14; i++)
        {
            int k = 0;
            for (int b = 0; b < 8; b++)
            {
                int bit = lfsr & 1;
                lfsr = (lfsr >> 1) ^ (bit != 0 ? 0xB400 : 0);
                k = (k << 1) | bit;
            }
            key[i] = (byte)k;
        }
        return key;
    }

    // ITU-T G.711 A-law to a 16 bit sample, the even bits inverted as
    // every A-law codec does it; the same table-free form the firmware's
    // encoder and the reference model use.
    private static int AlawDecode(byte a)
    {
        a ^= 0x55;
        int t = (a & 0xF) << 4;
        int seg = (a & 0x70) >> 4;
        if (seg == 0) t += 8;
        else if (seg == 1) t += 0x108;
        else { t += 0x108; t <<= seg - 1; }
        return (a & 0x80) != 0 ? t : -t;
    }

    private static byte Crc8(byte[] d, int start, int n)
    {
        int c = 0;
        for (int i = 0; i < n; i++)
        {
            c ^= d[start + i];
            for (int b = 0; b < 8; b++) c = (c & 0x80) != 0 ? ((c << 1) ^ 0x07) & 0xFF : (c << 1) & 0xFF;
        }
        return (byte)c;
    }

    // RS(13,9) over GF(256), x^8+x^4+x^3+x^2+1, g(x) = (x-1)(x-a)(x-a^2)
    // (x-a^3): four syndromes S_i = sum_j Y_j X_j^i with X_j = a^(n-1-p).
    // The code is small, so decoding is by trial, the same as the
    // reference model: the erasure positions plus every choice of t
    // further positions, t up to (4 - e) / 2, solved for the magnitudes
    // by Gaussian elimination; the first choice all four syndromes agree
    // with wins. Miscorrection of three errors measured at 3 in 20000
    // in the reference, mostly caught by the header check after.
    private static class Rs
    {
        private const int Npar = 4;
        private static readonly byte[] Exp = new byte[512];
        private static readonly byte[] Log = new byte[256];
        static Rs()
        {
            int x = 1;
            for (int i = 0; i < 255; i++)
            {
                Exp[i] = (byte)x;
                Log[x] = (byte)i;
                x <<= 1;
                if ((x & 0x100) != 0) x ^= 0x11D;
            }
            for (int i = 255; i < 512; i++) Exp[i] = Exp[i - 255];
        }
        private static int Mul(int a, int b) => (a == 0 || b == 0) ? 0 : Exp[Log[a] + Log[b]];
        private static int Inv(int a) => Exp[255 - Log[a]];

        // Bytes changed (0 = clean), or -1 if uncorrectable.
        public static int Decode(byte[] cw, int[] erasures)
        {
            int n = cw.Length;
            var S = new int[Npar];
            bool any = false;
            for (int i = 0; i < Npar; i++)
            {
                int acc = 0;
                foreach (byte c in cw) acc = Mul(acc, Exp[i]) ^ c;
                S[i] = acc;
                any |= acc != 0;
            }
            if (!any) return 0;
            int e = erasures.Length;
            if (e > Npar) return -1;
            if (e == 0) return DecodeErrors(cw, S);
            var others = new List<int>();
            for (int q = 0; q < n; q++) if (Array.IndexOf(erasures, q) < 0) others.Add(q);
            var pos = new int[Npar];
            Array.Copy(erasures, pos, e);
            for (int t = 0; t <= (Npar - e) / 2; t++)
            {
                if (t == 0)
                {
                    if (TrySolve(cw, S, pos, e, out int ch)) return ch;
                    continue;
                }
                if (t == 1)
                {
                    foreach (int a in others)
                    {
                        pos[e] = a;
                        if (TrySolve(cw, S, pos, e + 1, out int ch)) return ch;
                    }
                    continue;
                }
                for (int i = 0; i < others.Count; i++)
                for (int j = i + 1; j < others.Count; j++)
                {
                    pos[e] = others[i]; pos[e + 1] = others[j];
                    if (TrySolve(cw, S, pos, e + 2, out int ch)) return ch;
                }
            }
            return -1;
        }

        // Errors only, in closed form (Peterson-Gorenstein-Zierler for
        // t <= 2), the same answer the trial over positions gives but
        // without 92 eliminations a lost row: one error has X = S1/S0,
        // Y = S0; two errors have the locator x^2 + s1 x + s2 with
        // s1 = (S1 S2 + S0 S3) / D, s2 = (S2^2 + S1 S3) / D,
        // D = S1^2 + S0 S2, its roots found among the 13 positions and
        // the magnitudes from S0 and S1; S2 and S3 must then agree.
        private static int DecodeErrors(byte[] cw, int[] S)
        {
            int n = cw.Length;
            // One error: S_i = Y X^i, so X = S1 / S0 and S2, S3 must follow.
            if (S[0] != 0 && S[1] != 0)
            {
                int x = Mul(S[1], Inv(S[0]));
                if (Mul(S[1], x) == S[2] && Mul(S[2], x) == S[3])
                {
                    int p = n - 1 - Log[x];
                    if (p >= 0 && p < n) { cw[p] ^= (byte)S[0]; return 1; }
                }
            }
            // Two errors.
            int d = Mul(S[1], S[1]) ^ Mul(S[0], S[2]);
            if (d == 0) return -1;
            int di = Inv(d);
            int s1 = Mul(Mul(S[1], S[2]) ^ Mul(S[0], S[3]), di);
            int s2 = Mul(Mul(S[2], S[2]) ^ Mul(S[1], S[3]), di);
            int r1 = -1, r2 = -1;
            for (int p = 0; p < n; p++)
            {
                int x = Exp[(n - 1 - p) % 255];
                if ((Mul(x, x) ^ Mul(s1, x) ^ s2) == 0)
                {
                    if (r1 < 0) r1 = p; else if (r2 < 0) r2 = p; else return -1;
                }
            }
            if (r1 < 0 || r2 < 0) return -1;
            int x1 = Exp[(n - 1 - r1) % 255], x2 = Exp[(n - 1 - r2) % 255];
            int y1 = Mul(S[1] ^ Mul(S[0], x2), Inv(x1 ^ x2));
            int y2 = S[0] ^ y1;
            if (y1 == 0 || y2 == 0) return -1;
            if ((Mul(y1, Mul(x1, x1)) ^ Mul(y2, Mul(x2, x2))) != S[2]) return -1;
            if ((Mul(y1, Mul(x1, Mul(x1, x1))) ^ Mul(y2, Mul(x2, Mul(x2, x2)))) != S[3]) return -1;
            cw[r1] ^= (byte)y1;
            cw[r2] ^= (byte)y2;
            return 2;
        }

        private static bool TrySolve(byte[] cw, int[] S, int[] pos, int k, out int changed)
        {
            changed = 0;
            int n = cw.Length;
            // rows[i] = [X_0^i .. X_{k-1}^i | S_i]
            var rows = new int[Npar, k + 1];
            for (int j = 0; j < k; j++)
            {
                int lx = (n - 1 - pos[j]) % 255;
                for (int i = 0; i < Npar; i++) rows[i, j] = Exp[(lx * i) % 255];
            }
            for (int i = 0; i < Npar; i++) rows[i, k] = S[i];
            int r = 0;
            for (int c = 0; c < k; c++)
            {
                int pr = -1;
                for (int i = r; i < Npar; i++) if (rows[i, c] != 0) { pr = i; break; }
                if (pr < 0) return false;
                if (pr != r)
                    for (int j = 0; j <= k; j++) (rows[r, j], rows[pr, j]) = (rows[pr, j], rows[r, j]);
                int inv = Inv(rows[r, c]);
                for (int j = 0; j <= k; j++) rows[r, j] = Mul(rows[r, j], inv);
                for (int i = 0; i < Npar; i++)
                {
                    if (i == r || rows[i, c] == 0) continue;
                    int f = rows[i, c];
                    for (int j = 0; j <= k; j++) rows[i, j] ^= Mul(f, rows[r, j]);
                }
                r++;
            }
            for (int i = r; i < Npar; i++) if (rows[i, k] != 0) return false;
            for (int j = 0; j < k; j++)
            {
                int y = rows[j, k];
                if (y != 0) { cw[pos[j]] ^= (byte)y; changed++; }
            }
            return true;
        }
    }

    // The whole 20 bit preamble at once: for each of the two bit periods
    // (with a small correction either way) and each start pixel, the 8
    // run-in bits must alternate (either polarity, and with real
    // contrast: a flat noisy signal could pass by chance with a
    // degenerate threshold), the 4 guard bits be low and the sync byte
    // match with at most one bit wrong, all sliced at the threshold the
    // run-in itself gives, (min + max) / 2. Scored by the smallest
    // distance any of the 20 bits kept from the threshold, an exact sync
    // above a one-bit one; the best score wins and the sync search of
    // earlier versions is gone with it. Locking the run-in alone was
    // not enough once two periods exist: 8 bits cannot tell 4.0 from
    // 4.3 px, and a wrong period read the row as the wrong format; over
    // 20 bits (up to 95 px) the wrong one drifts out of its cells.
    // The 20 bit preamble at one start and period: 8 alternating run-in
    // bits (either polarity, with real contrast), 4 low guard bits and
    // the sync byte with at most one bit wrong, sliced at the threshold
    // the run-in gives; the score is the smallest margin, an exact sync
    // ranking above a one-bit one.
    private static bool Preamble(byte[] pixels, int rowOffset, int width, int p0, double p, Span<int> y,
        out int threshold, out double sStart, out double score)
    {
        threshold = 0; sStart = 0; score = 0;
        if (p0 + (PreambleBits + GuardBits + 8) * p > width) return false;
        int lo = 255, hi = 0;
        for (int i = 0; i < PreambleBits; i++)
        {
            y[i] = YAtPixel(pixels, rowOffset, (int)Math.Round(p0 + i * p));
            if (y[i] < lo) lo = y[i];
            if (y[i] > hi) hi = y[i];
        }
        if (hi - lo < 40) return false;
        threshold = (lo + hi) / 2;
        int first = y[0] > threshold ? 1 : 0;
        int margin = int.MaxValue;
        for (int i = 0; i < PreambleBits; i++)
        {
            if ((y[i] > threshold ? 1 : 0) != ((first + i) & 1)) return false;
            margin = Math.Min(margin, Math.Abs(y[i] - threshold));
        }
        for (int i = 0; i < GuardBits; i++)
        {
            int v = YAtPixel(pixels, rowOffset, (int)Math.Round(p0 + (PreambleBits + i) * p));
            if (v > threshold) return false;
            margin = Math.Min(margin, threshold - v);
        }
        sStart = p0 + (PreambleBits + GuardBits) * p;
        int b = 0;
        for (int i = 0; i < 8; i++)
        {
            int v = YAtPixel(pixels, rowOffset, (int)Math.Round(sStart + i * p));
            b = (b << 1) | (v > threshold ? 1 : 0);
            margin = Math.Min(margin, Math.Abs(v - threshold));
        }
        int wrong = System.Numerics.BitOperations.PopCount((uint)(b ^ SyncByte));
        if (wrong > 1) return false;
        score = (wrong == 0 ? 1000 : 0) + margin;
        return true;
    }

    private static List<Candidate> FindPreamble(byte[] pixels, int rowOffset, int width)
    {
        // Cheap first: the start pixel is found at the nominal periods
        // alone (a wrong start fails on its first or second bit), and
        // only a start that passes gets the period refined around it.
        // Searching period and start together for every row cost more
        // than a frame and made the sound stutter.
        var found = new List<Candidate>();
        Span<int> y = stackalloc int[PreambleBits];
        foreach (double nominal in Periods)
        {
            // Every start pixel that passes at the nominal period forms a
            // run a few pixels wide (the cell); its middle is the sample
            // point with margin either side. Taking the best-scoring
            // pixel instead picked a cell edge whenever margins tied,
            // and from an edge the row's tail drifted into the next cell
            // (found by the self test's stress rows, then on the board).
            int runStart = -1, runLen = 0, bestStart = -1, bestLen = 0;
            for (int p0 = 0; p0 <= MaxSearchPixels; p0++)
            {
                bool pass = p0 < MaxSearchPixels && Preamble(pixels, rowOffset, width, p0, nominal, y, out _, out _, out _);
                if (pass)
                {
                    if (runStart < 0) { runStart = p0; runLen = 0; }
                    runLen++;
                }
                else if (runStart >= 0)
                {
                    if (runLen > bestLen) { bestLen = runLen; bestStart = runStart; }
                    runStart = -1;
                }
            }
            if (bestLen == 0) continue;
            int centre = bestStart + (bestLen - 1) / 2;
            Candidate? best = null;
            for (double dp = -PeriodRange; dp <= PeriodRange + 1e-9; dp += PeriodStep)
            {
                double p = nominal + dp;
                if (p < PeriodMin || p > PeriodMax) continue;
                if (!Preamble(pixels, rowOffset, width, centre, p, y, out int threshold, out double sStart, out double score)) continue;
                double sc = score - PeriodPenalty * Math.Abs(dp);
                if (best == null || sc > best.Value.Score) best = new Candidate(centre, p, threshold, sStart, sc);
            }
            if (best != null) found.Add(best.Value);
        }
        // Best first; on equal scores the one nearer its nominal period.
        found.Sort((u, v) =>
        {
            int c = v.Score.CompareTo(u.Score);
            if (c != 0) return c;
            double du = Math.Abs(u.Period - Periods[FamilyOf(u.Period)]);
            double dv = Math.Abs(v.Period - Periods[FamilyOf(v.Period)]);
            return du.CompareTo(dv);
        });
        // Two are enough: the true one scores best, and trying more only
        // cost time on rows that are lost anyway.
        if (found.Count > 2) found.RemoveRange(2, found.Count - 2);
        return found;
    }
}

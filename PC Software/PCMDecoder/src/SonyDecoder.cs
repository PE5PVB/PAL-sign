// Decodes the EIAJ STC-007 / Sony PCM-F1 format written by
// Firmware/src/testcards/sonypcm.cpp (the format itself is documented
// there, with sources). One field at a time: the control line, then
// 294 data lines, each 137 bits at 36/7 pixels a bit -- a data sync
// "1010", eight 14 bit words, a 16 bit CRC, a "0" and four "1"s of
// white reference. The words of one audio block are spread over 113
// lines (D = 16 interleave), so a block is complete 112 lines after its
// first word; this keeps that history and emits 3 stereo samples per
// completed block, 44.1 kHz.
//
// Error handling here is deliberately modest: a line whose CRC fails
// is marked bad, a block with exactly one bad sample word and a good P
// is repaired with P, anything worse holds the previous sample. Q
// correction (a second erasure) is not implemented; this is a monitor
// for a clean composite link, not a tape player. Fagear's
// SDVPCMdecoder is the reference for the full thing.
using System;
using System.Collections.Generic;

namespace PcmDecoder;

public sealed class SonyDecoder
{
    // Nominal geometry in this tool's pixels: the firmware starts the
    // data sync at its active pixel 1.7 (bit 26 of 168 at 36/7 px a
    // bit), and the capture card's row starts ~6 pixels before the
    // firmware's pixel 0 (measured with the board's own Ham PCM card:
    // its pixel 0 run-in locks at 6 here). Searched around, not assumed.
    private const double PixelsPerBit = 36.0 / 7.0;
    // Measured 0.2 on the Intensity Pro, not the 7.7 (firmware pixel 1.7
    // plus the ~6 pixel window offset seen with the board's own card)
    // first assumed: the firmware's active pixel 0 is not 132 pixels
    // after 0H as taken from BT.601, or the card's window is not where
    // that measurement put it -- a scope on the CVBS output decides.
    private const double NominalStart = 0.5;
    private const int SearchPixels = 14;
    private const double SearchStep = 0.5;
    private const int LineBits = 137;
    private const int Depth = 113;

    public const int SampleRate = 44100;

    // Per field: the control line, then the data lines, per IEC
    // 60841:1988 clause 8.3 / figure 4b for 625/50: control on the 6th
    // line of the first field and the 6.5th of the second, 294 data
    // lines after it. Line numbers are the frame's (1..625).
    // EXPERIMENT, in step with sonypcm.cpp: 10 lines later than the
    // figure, so a 576 line capture misses 7 lines a field, not 17, and
    // the last data line stays clear of the encoder's blanking.
    public const int CtrlLineF1 = 16, DataFirstF1 = 17, DataLastF1 = 310;
    public const int CtrlLineF2 = 329, DataFirstF2 = 330, DataLastF2 = 623;
    // The card's "control line in picture" option: control on the first
    // captured row, data 294 lines on, the last 8 a field in the blanking.
    public const int PicCtrlF1 = 24, PicDataFirstF1 = 25, PicDataLastF1 = 318;
    public const int PicCtrlF2 = 337, PicDataFirstF2 = 338, PicDataLastF2 = 631;
    public bool InPicture { get; private set; }

    public long LinesOk, LinesNoSync, LinesCrcBad, LinesMissing;
    public long BlocksClean, BlocksRepaired, BlocksRepaired2, BlocksHeld;
    public bool Mode16 { get; private set; }
    public bool ControlSeen { get; private set; }
    // Per PAL line (1..625) of the last frame: 0 not asked or missing,
    // 1 decoded, 3 unreadable (no sync or CRC), for the row map.
    public readonly byte[] LineStatus = new byte[640];   // 626..631: the in-picture placement's tail

    private struct Line
    {
        public ushort[] W;   // 8 words
        public bool Valid;
    }
    private readonly Line[] _hist = new Line[Depth];
    private long _lineSeq;   // data lines received, continuous across fields
    private short _lastL, _lastR;

    // A source of line pixels by absolute line number: the caller maps
    // the capture card's rows and vertical-blanking buffers onto it.
    // Returns false when the card did not deliver that line.
    public delegate bool LineSource(int line, out byte[] pixels, out int offset, out int rowBytes);

    // Decodes both fields of one frame, in transmission order, and
    // returns the samples completed by them.
    public List<SamplePair> DecodeFrame(LineSource src)
    {
        var outp = new List<SamplePair>(2000);
        // Placement by whether the cueing word sits on line 24.
        InPicture = src(PicCtrlF1, out var cp, out int co, out int crb) && ReadLine(cp, co, crb, out var cw) &&
                    cw[0] == 0x3333 && cw[1] == 0x0CCC && cw[2] == 0x3333 && cw[3] == 0x0CCC;
        if (InPicture)
        {
            DecodeField(src, PicCtrlF1, PicDataFirstF1, PicDataLastF1, outp);
            DecodeField(src, PicCtrlF2, PicDataFirstF2, PicDataLastF2, outp);
        }
        else
        {
            DecodeField(src, CtrlLineF1, DataFirstF1, DataLastF1, outp);
            DecodeField(src, CtrlLineF2, DataFirstF2, DataLastF2, outp);
        }
        return outp;
    }

    private void DecodeField(LineSource src, int ctrl, int first, int last, List<SamplePair> outp)
    {
        if (src(ctrl, out var cp, out int co, out int crb) && ReadLine(cp, co, crb, out var cw))
        {
            // Cueing word 1100... as 0x3333 0x0CCC 0x3333 0x0CCC, ID 0.
            if (cw[0] == 0x3333 && cw[1] == 0x0CCC && cw[2] == 0x3333 && cw[3] == 0x0CCC && cw[4] == 0)
            {
                ControlSeen = true;
                Mode16 = (cw[7] & 0x0002) != 0;   // Q "not applied": the 16 bit S word instead
            }
        }
        for (int line = first; line <= last; line++)
        {
            var entry = new Line { W = new ushort[8], Valid = false };
            LineStatus[line] = 3;
            if (!src(line, out var px, out int off, out int rb))
            {
                LinesMissing++;
                LineStatus[line] = 0;
            }
            else if (ReadLine(px, off, rb, out var w))
            {
                entry.W = w;
                entry.Valid = true;
                LinesOk++;
                LineStatus[line] = 1;
            }
            _hist[_lineSeq % Depth] = entry;
            EmitBlock(outp);
            _lineSeq++;
        }
    }

    // Block k is complete when line k + 112 (its Q/S) has arrived: L0
    // from line k, R0 from k+16, L1 k+32, R1 k+48, L2 k+64, R2 k+80,
    // P k+96, Q/S k+112 (word 0..7 of those lines respectively).
    private void EmitBlock(List<SamplePair> outp)
    {
        if (_lineSeq < Depth - 1) return;
        long k = _lineSeq - 112;
        var l0 = _hist[(k) % Depth]; var r0 = _hist[(k + 16) % Depth];
        var l1 = _hist[(k + 32) % Depth]; var r1 = _hist[(k + 48) % Depth];
        var l2 = _hist[(k + 64) % Depth]; var r2 = _hist[(k + 80) % Depth];
        var p = _hist[(k + 96) % Depth]; var q = _hist[(k + 112) % Depth];
        ushort[] w = { l0.W[0], r0.W[1], l1.W[2], r1.W[3], l2.W[4], r2.W[5] };
        bool[] ok = { l0.Valid, r0.Valid, l1.Valid, r1.Valid, l2.Valid, r2.Valid };
        int bad = 0, badA = -1, badB = -1;
        for (int i = 0; i < 6; i++) if (!ok[i]) { bad++; if (badA < 0) badA = i; else badB = i; }

        // Erasure correction with P and Q (IEC 60841 clause 11.3):
        //   P = w0 ^ w1 ^ ... ^ w5
        //   Q = T^6 w0 ^ T^5 w1 ^ T^4 w2 ^ T^3 w3 ^ T^2 w4 ^ T w5
        // A missing word's position is known (its line failed), so one
        // missing word follows from P alone or from Q alone, and two
        // from both together: with Sp = P ^ (the good words) and Sq =
        // Q ^ (the good words' T terms), (T^a + T^b) w_a = Sq + T^b Sp.
        // In 16 bit mode the Q slot holds the S word, so P only.
        bool qUsable = q.Valid && !Mode16;
        if (bad == 1 && p.Valid)
        {
            ushort x = p.W[6];
            for (int i = 0; i < 6; i++) if (i != badA) x ^= w[i];
            w[badA] = x;
            bad = 0;
            BlocksRepaired++;
        }
        else if (bad == 1 && qUsable)
        {
            ushort sq = q.W[7];
            for (int i = 0; i < 6; i++) if (i != badA) sq ^= Gf2.Pow(6 - i, w[i]);
            w[badA] = Gf2.Apply(Gf2.InvPow(6 - badA), sq);
            bad = 0;
            BlocksRepaired++;
        }
        else if (bad == 2 && p.Valid && qUsable)
        {
            ushort sp = p.W[6], sq = q.W[7];
            for (int i = 0; i < 6; i++)
            {
                if (i == badA || i == badB) continue;
                sp ^= w[i];
                sq ^= Gf2.Pow(6 - i, w[i]);
            }
            int a = 6 - badA, b = 6 - badB;
            ushort rhs = (ushort)(sq ^ Gf2.Pow(b, sp));
            w[badA] = Gf2.Apply(Gf2.InvSumPow(a, b), rhs);
            w[badB] = (ushort)(sp ^ w[badA]);
            bad = 0;
            BlocksRepaired2++;
        }
        else if (bad == 0) BlocksClean++;

        // 14 or 16 bit without the control block (this capture card gives
        // no blanking lines): in 14 bit the Q slot is the T-matrix parity
        // of the six words, in 16 bit it is the S word of low bits and
        // never matches. Decided on a clean block's own words, by majority.
        if (bad == 0 && q.Valid)
        {
            ushort qc = MatT(w[0]);
            qc = MatT((ushort)(qc ^ w[1])); qc = MatT((ushort)(qc ^ w[2]));
            qc = MatT((ushort)(qc ^ w[3])); qc = MatT((ushort)(qc ^ w[4]));
            qc = MatT((ushort)(qc ^ w[5]));
            if (qc == q.W[7]) _qMatch++; else _qMismatch++;
            if (!ControlSeen && _qMatch + _qMismatch >= 200)
            {
                Mode16 = _qMismatch > _qMatch;
                _qMatch = _qMismatch = 0;
            }
        }

        for (int i = 0; i < 3; i++)
        {
            short l, r;
            if (bad == 0)
            {
                int li = w[2 * i] << 2, ri = w[2 * i + 1] << 2;   // 14 bit to 16 bit
                if (Mode16 && q.Valid)
                {
                    int s = q.W[7];
                    li |= (s >> (12 - 4 * i)) & 3;
                    ri |= (s >> (10 - 4 * i)) & 3;
                }
                l = (short)li;
                r = (short)ri;
                _lastL = l;
                _lastR = r;
            }
            else
            {
                l = _lastL;
                r = _lastR;
                if (i == 0) BlocksHeld++;
            }
            outp.Add(new SamplePair(l, r));
        }
    }

    // One line: find the data sync and the stop marker together, then
    // read 8 words + CRC from the bit centres.
    private bool ReadLine(byte[] px, int off, int rowBytes, out ushort[] words)
    {
        words = new ushort[8];
        int width = rowBytes / 2;
        // Threshold from the line's own extremes over the data span.
        int lo = 255, hi = 0;
        int spanEnd = Math.Min(width - 1, (int)(NominalStart + LineBits * PixelsPerBit) + SearchPixels);
        for (int x = 0; x < spanEnd; x++)
        {
            int y = RowDecoder.YAtPixel(px, off, x);
            if (y < lo) lo = y;
            if (y > hi) hi = y;
        }
        if (hi - lo < 40) { LinesNoSync++; return false; }
        int thr = (lo + hi) / 2;

        // The start that worked last time is tried first, exactly and
        // then a pixel either way; the full search only when that fails.
        // Searching every line from scratch (57 starts x 137 bits) cost
        // 140 ms a frame and dropped two frames in three.
        // The exact start that worked last time first, and it stays put
        // while it works: letting it move half a pixel whenever a
        // neighbour also passed the nine framing bits made it wander off
        // the bit centres over a few seconds (bursts of nosync lines).
        // When it fails, every candidate is scored and the best margin
        // wins, not the first that passes.
        bool locked = FramingMargin(px, off, width, thr, _lastStart) > 0;
        if (!locked)
        {
            double best = 0, bestStart = 0;
            for (double d = 0; d <= SearchPixels; d += SearchStep)
            {
                foreach (double start in d == 0 ? new[] { NominalStart } : new[] { NominalStart + d, NominalStart - d })
                {
                    double m = FramingMargin(px, off, width, thr, start);
                    if (m > best) { best = m; bestStart = start; }
                }
            }
            if (best <= 0) { LinesNoSync++; return false; }
            _lastStart = bestStart;
        }
        ReadBits(px, off, width, thr, _lastStart);
        int[] bits = _bits;

        int n = 4;
        for (int i = 0; i < 8; i++)
        {
            int v = 0;
            for (int b = 0; b < 14; b++) v = (v << 1) | bits[n++];
            words[i] = (ushort)v;
        }
        int crc = 0;
        for (int b = 0; b < 16; b++) crc = (crc << 1) | bits[n++];
        if (Crc(words) != crc) { LinesCrcBad++; return false; }
        return true;
    }

    private long _qMatch, _qMismatch;

    // The norm's Q generating matrix: a shift left with bit 13 fed back
    // into bits 0 and 8 (x^14 + x^8 + 1), as in the firmware.
    private static ushort MatT(ushort w) => Gf2.T(w);

    // 14x14 matrices over GF(2) for the erasure algebra: T^k, its
    // inverse, and (T^a + T^b)^-1, built once by Gaussian elimination.
    private static class Gf2
    {
        public static ushort T(ushort w) =>
            (ushort)(((w << 1) & 0x3FFF) ^ ((w & 0x2000) != 0 ? 0x0101 : 0));

        public static ushort Pow(int k, ushort w)
        {
            for (int i = 0; i < k; i++) w = T(w);
            return w;
        }

        // A matrix as 14 columns: column c is M applied to the unit
        // vector with only bit c set. Apply = XOR of the columns of the
        // set bits.
        public static ushort Apply(ushort[] m, ushort w)
        {
            ushort r = 0;
            for (int c = 0; c < 14; c++) if ((w & (1 << c)) != 0) r ^= m[c];
            return r;
        }

        private static ushort[] PowMatrix(int k)
        {
            var m = new ushort[14];
            for (int c = 0; c < 14; c++) m[c] = Pow(k, (ushort)(1 << c));
            return m;
        }

        private static ushort[] Invert(ushort[] m)
        {
            // Rows of [M | I] as 28 bit values, eliminate to [I | M^-1].
            // Row r of M: bit c is bit r of column c.
            var rows = new uint[14];
            for (int r = 0; r < 14; r++)
            {
                uint v = 0;
                for (int c = 0; c < 14; c++) if ((m[c] & (1 << r)) != 0) v |= 1u << c;
                rows[r] = v | (1u << (14 + r));
            }
            for (int c = 0; c < 14; c++)
            {
                int pivot = -1;
                for (int r = c; r < 14; r++) if ((rows[r] & (1u << c)) != 0) { pivot = r; break; }
                if (pivot < 0) throw new InvalidOperationException("singular matrix");
                (rows[c], rows[pivot]) = (rows[pivot], rows[c]);
                for (int r = 0; r < 14; r++) if (r != c && (rows[r] & (1u << c)) != 0) rows[r] ^= rows[c];
            }
            // Column c of the inverse: bit r = bit (14 + c) of row r.
            var inv = new ushort[14];
            for (int c = 0; c < 14; c++)
            {
                ushort col = 0;
                for (int r = 0; r < 14; r++) if ((rows[r] & (1u << (14 + c))) != 0) col |= (ushort)(1 << r);
                inv[c] = col;
            }
            return inv;
        }

        private static readonly Dictionary<int, ushort[]> _invPow = new();
        private static readonly Dictionary<int, ushort[]> _invSum = new();

        public static ushort[] InvPow(int k)
        {
            if (!_invPow.TryGetValue(k, out var m)) _invPow[k] = m = Invert(PowMatrix(k));
            return m;
        }

        public static ushort[] InvSumPow(int a, int b)
        {
            int key = a * 16 + b;
            if (!_invSum.TryGetValue(key, out var m))
            {
                var pa = PowMatrix(a);
                var pb = PowMatrix(b);
                var sum = new ushort[14];
                for (int c = 0; c < 14; c++) sum[c] = (ushort)(pa[c] ^ pb[c]);
                _invSum[key] = m = Invert(sum);
            }
            return m;
        }
    }

    private double _lastStart = NominalStart;
    private readonly int[] _bits = new int[LineBits];

    private static readonly int[] FramingBit = { 0, 1, 2, 3, 132, 133, 134, 135, 136 };
    private static readonly int[] FramingWant = { 1, 0, 1, 0, 0, 1, 1, 1, 1 };

    // How convincingly the nine framing bits ("1010" and "0 1111") read
    // right from `start`: the smallest distance of any of them to the
    // threshold, 0 or less when one reads wrong.
    private static double FramingMargin(byte[] px, int off, int width, int thr, double start)
    {
        double worst = double.MaxValue;
        for (int k = 0; k < FramingBit.Length; k++)
        {
            int x = (int)Math.Round(start + (FramingBit[k] + 0.5) * PixelsPerBit);
            if (x < 0 || x >= width) return 0;
            int y = RowDecoder.YAtPixel(px, off, x);
            double m = FramingWant[k] == 1 ? y - thr : thr - y;
            if (m <= 0) return 0;
            if (m < worst) worst = m;
        }
        return worst;
    }

    // Samples the 137 bit centres from `start` into _bits.
    private void ReadBits(byte[] px, int off, int width, int thr, double start)
    {
        int[] bits = _bits;
        for (int i = 0; i < LineBits; i++)
        {
            int x = (int)Math.Round(start + (i + 0.5) * PixelsPerBit);
            bits[i] = (x < 0 || x >= width) ? 0 : (RowDecoder.YAtPixel(px, off, x) > thr ? 1 : 0);
        }
    }

    public double LastStart => _lastStart;

    // CRC-16/CCITT-FALSE over the 112 data bits, MSB first: the norm's
    // x^16 + x^12 + x^5 + 1 with the first 16 bits inverted.
    private static int Crc(ushort[] w)
    {
        int crc = 0xFFFF;
        for (int i = 0; i < 8; i++)
        {
            for (int b = 13; b >= 0; b--)
            {
                bool top = (((crc >> 15) ^ (w[i] >> b)) & 1) != 0;
                crc = (crc << 1) & 0xFFFF;
                if (top) crc ^= 0x1021;
            }
        }
        return crc;
    }
}

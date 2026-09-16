// Fetching a card off the board as a picture, and keeping it. Key X
// renders the card and sends 576 rows of 720 samples in 4:2:2, exactly
// the pixel bus bytes, so the picture is never redrawn here. Loading a
// card to render it blacks the television, so each is kept on disk under
// its CRC (from the board's settings dump, over the firmware build, the
// card's settings and the shared texts; see cfgCardCrc()) and only
// re-fetched when that CRC changes. The clock is deliberately not in
// that CRC, so the three cards with insert boxes can be cached too; their
// copy carries the time it was fetched at.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace PalSign;

/// <summary>Where the insert boxes of a card are, in picture
/// coordinates, and which of them carry a clock. The board catches these
/// as the card passes them to insertPrepare(), so no card has to declare
/// them anywhere.</summary>
public record CardBoxes(int Lx0, int Lx1, int Rx0, int Rx1, int Y0, int Y1, int Mode,
                        int Mx0 = 0, int Mx1 = 0, int My0 = 0, int My1 = 0)
{
    public static readonly CardBoxes None = new(0, 0, 0, 0, 0, 0, 0);

    // 1 is the time on the right, 2 is the date on the left as well.
    // 0 has no boxes and 3 carries fixed text, which does not change and
    // is therefore left in the picture the board sends.
    public bool HasClock => Y1 > Y0 && (Mode == 1 || Mode == 2);
    public bool HasDate => HasClock && Mode == 2;

    // The box the moving line sweeps through; the stripe itself moves,
    // so it is not in the board's picture and the preview animates it.
    public bool HasLine => Mx1 > Mx0;
}

public static class CardCache
{
    public const int W = 720, H = 576;

    private static string Folder =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                     "PAL-sign", "cards");

    private static string FileOf(int number, uint crc) =>
        Path.Combine(Folder, $"card-{number:D2}-{crc:X8}.png");

    // The box rectangles, in a file of their own: too many numbers for
    // the file name.
    private static string BoxOf(int number, uint crc) =>
        Path.Combine(Folder, $"card-{number:D2}-{crc:X8}.box");

    /// <summary>Whatever is kept for this card, with the CRC it was
    /// fetched under (at most one; saving throws the others away). Found
    /// by card number and the CRC in the file name, since at startup
    /// there is no board yet to ask; Connect compares that CRC with the
    /// board's own before deciding to fetch.</summary>
    public static (Image? picture, uint crc, CardBoxes boxes) LoadAny(int number)
    {
        if (!Directory.Exists(Folder)) return (null, 0, CardBoxes.None);
        foreach (string f in Directory.GetFiles(Folder, $"card-{number:D2}-*.png"))
        {
            string tag = Path.GetFileNameWithoutExtension(f);
            int dash = tag.LastIndexOf('-');
            if (dash < 0) continue;
            if (!uint.TryParse(tag[(dash + 1)..], NumberStyles.HexNumber,
                               CultureInfo.InvariantCulture, out uint crc)) continue;
            var img = Read(f);
            if (img != null) return (img, crc, ReadBoxes(BoxOf(number, crc)));
        }
        return (null, 0, CardBoxes.None);
    }

    private static Image? Read(string f)
    {
        if (!File.Exists(f)) return null;
        try
        {
            // Read into memory and close the file: Image.FromFile keeps the
            // file locked for as long as the picture lives, and these are
            // held for the whole session.
            byte[] raw = File.ReadAllBytes(f);
            using var mem = new MemoryStream(raw);
            return new Bitmap(mem);
        }
        catch (Exception) { return null; }
    }

    public static void Save(int number, uint crc, Image picture, CardBoxes boxes)
    {
        Directory.CreateDirectory(Folder);
        // Anything kept for this card under another CRC is out of date.
        foreach (string old in Directory.GetFiles(Folder, $"card-{number:D2}-*.*"))
        {
            try { File.Delete(old); } catch { /* allowed to fail */ }
        }
        picture.Save(FileOf(number, crc), ImageFormat.Png);
        File.WriteAllText(BoxOf(number, crc),
                          $"{boxes.Lx0} {boxes.Lx1} {boxes.Rx0} {boxes.Rx1} " +
                          $"{boxes.Y0} {boxes.Y1} {boxes.Mode} " +
                          $"{boxes.Mx0} {boxes.Mx1} {boxes.My0} {boxes.My1}");
    }

    private static CardBoxes ReadBoxes(string f)
    {
        if (!File.Exists(f)) return CardBoxes.None;
        try
        {
            string[] p = File.ReadAllText(f).Split(' ', StringSplitOptions.RemoveEmptyEntries);
            // Seven numbers: a cache file from before the line box field.
            // Parses with an empty line box; FetchPictures re-fetches such
            // a card once if the board now says it has one.
            if (p.Length < 7) return CardBoxes.None;
            var v = new int[11];
            int n = p.Length >= 11 ? 11 : 7;
            for (int i = 0; i < n; i++)
                if (!int.TryParse(p[i], out v[i])) return CardBoxes.None;
            return new CardBoxes(v[0], v[1], v[2], v[3], v[4], v[5], v[6],
                                 v[7], v[8], v[9], v[10]);
        }
        catch (Exception) { return CardBoxes.None; }
    }

    public static void Forget()
    {
        if (!Directory.Exists(Folder)) return;
        foreach (string f in Directory.GetFiles(Folder, "card-*.*"))
        {
            try { File.Delete(f); } catch { /* allowed to fail */ }
        }
    }

    // --- off the board -----------------------------------------------------

    /// <summary>A port to fetch a run of cards over. Opening and closing
    /// one costs tens of milliseconds and there are 33 cards, so it is
    /// worth holding on to for the length of a Connect.</summary>
    public static SerialPort OpenFetch(string portName)
    {
        var port = new SerialPort(portName, 115200)
        {
            ReadTimeout = 8000,
            WriteTimeout = 8000,
            NewLine = "\n",
            DtrEnable = true,
            RtsEnable = true,
            // Large on purpose: the board gives up after a second without
            // progress, so this covers a read hiccup on this side.
            ReadBufferSize = 1 << 18,
        };
        port.Open();
        return port;
    }

    public static void CloseFetch(SerialPort port)
    {
        try { port.Close(); } catch { /* allowed to fail */ }
    }

    /// <summary>Renders the card on the board and reads the picture back;
    /// the television goes black while it happens. progress is called
    /// with 0 to 100. Call from a worker thread, not the drawing thread:
    /// the board gives up if this side stops reading for a second, which
    /// a message pump would cause.</summary>
    public static (Bitmap picture, CardBoxes boxes) Fetch(SerialPort port, int number,
                                                          Action<int>? progress,
                                                          CancellationToken stop)
    {
        {
            port.DiscardInBuffer();
            var pump = new Pump(port);
            Trace.Say($"card {number}: sending X");
            port.Write("X");
            WaitOn(pump, "[carddump] ready", stop);
            Trace.Line("send", number.ToString(CultureInfo.InvariantCulture) + "\n");
            port.Write(number.ToString(CultureInfo.InvariantCulture) + "\n");
            WaitOn(pump, "[carddump] card", stop);
            CardBoxes boxes = TakeBoxes(WaitOn(pump, "[carddump] boxes", stop));
            boxes = TakeLineBox(WaitOn(pump, "[carddump] linebox", stop), boxes);

            byte[] raw = ReadRuns(pump, progress, stop);
            Trace.Say("the runs are in, waiting for done");
            WaitOn(pump, "[carddump] done", stop);
            Trace.Say($"card {number} complete");
            return (ToBitmap(raw), boxes);
        }
    }

    // "[carddump] boxes lx0 lx1 rx0 rx1 y0 y1 mode N", or "boxes none".
    private static CardBoxes TakeBoxes(string line)
    {
        string[] p = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        // [carddump] boxes <six numbers> mode <n>
        if (p.Length < 10) return CardBoxes.None;
        var v = new int[6];
        for (int i = 0; i < 6; i++)
            if (!int.TryParse(p[i + 2], out v[i])) return CardBoxes.None;
        int mode = int.TryParse(p[9], out int m) ? m : 0;
        return new CardBoxes(v[0], v[1], v[2], v[3], v[4], v[5], mode);
    }

    // "[carddump] linebox x0 x1 y0 y1", or "linebox none".
    private static CardBoxes TakeLineBox(string line, CardBoxes boxes)
    {
        string[] p = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (p.Length < 6) return boxes;
        var v = new int[4];
        for (int i = 0; i < 4; i++)
            if (!int.TryParse(p[i + 2], out v[i])) return boxes;
        return boxes with { Mx0 = v[0], Mx1 = v[1], My0 = v[2], My1 = v[3] };
    }

    // Run length coded: a count of 1 to 255 and the 32 bit word (one
    // 4:2:2 pair) it repeats. No length in the header, since the board
    // would have to render the whole card to know it; the trailing
    // "done" line confirms both sides agree where the picture ended.
    //
    // One shared buffer for everything that comes back, not one per
    // block: a per-block buffer would let the trailing "done" line sit
    // in a block that then goes out of scope, so the wait for "done"
    // right after would read from a port that has already given it up.
    private sealed class Pump
    {
        private readonly SerialPort _port;
        private readonly byte[] _buf = new byte[4096];
        private int _have, _take;

        public Pump(SerialPort port) { _port = port; }

        /// <summary>The next byte, or -1 if nothing came before the
        /// deadline.</summary>
        public int Next(DateTime end, CancellationToken stop)
        {
            while (_take >= _have)
            {
                stop.ThrowIfCancellationRequested();
                if (DateTime.UtcNow > end) return -1;
                try { _have = _port.Read(_buf, 0, _buf.Length); }
                catch (TimeoutException) { _have = 0; continue; }
                _take = 0;
            }
            return _buf[_take++];
        }

        public string? Line(DateTime end, CancellationToken stop)
        {
            var line = new StringBuilder();
            for (;;)
            {
                int c = Next(end, stop);
                if (c < 0) return null;
                if (c == '\n') return line.ToString();
                if (c != '\r') line.Append((char)c);
            }
        }
    }

    private static string WaitOn(Pump pump, string start, CancellationToken stop, int seconds = 30)
    {
        var end = DateTime.UtcNow.AddSeconds(seconds);
        for (;;)
        {
            string? r = pump.Line(end, stop);
            if (r == null) break;
            Trace.Line("recv", r);
            {
                string? bad = BoardWords.Refusal(r);
                if (bad != null) throw new UploadError(bad);
            }
            if (r.StartsWith(start, StringComparison.Ordinal)) return r;
        }
        Trace.Say($"NOTHING that began with \"{start}\" came back within {seconds} s");
        throw new UploadError($"no reply to \"{start}\" within {seconds} s");
    }

    private static byte[] ReadRuns(Pump pump, Action<int>? progress, CancellationToken stop)
    {
        var outp = new byte[W * 2 * H];
        int at = 0;                       // bytes written
        var end = DateTime.UtcNow.AddSeconds(60);
        int told = -1;

        byte Next()
        {
            int c = pump.Next(end, stop);
            if (c < 0)
            {
                Trace.Say($"the run stream stopped at {at} of {outp.Length} bytes");
                throw new UploadError($"only {at} of {outp.Length} bytes arrived");
            }
            end = DateTime.UtcNow.AddSeconds(20);   // it is moving, so wait again
            return (byte)c;
        }

        while (at < outp.Length)
        {
            int run = Next();
            byte b0 = Next(), b1 = Next(), b2 = Next(), b3 = Next();
            if (run < 1) throw new UploadError("a run of no length: the stream is out of step");
            // An overrunning run is an error, not something to trim: a
            // silently trimmed stream would be cached as complete under
            // the card's checksum, and never fetched again.
            if (at + run * 4 > outp.Length)
                throw new UploadError("the card is longer than a card can be: the stream is out "
                                      + "of step");
            for (int i = 0; i < run; i++)
            {
                outp[at++] = b0;
                outp[at++] = b1;
                outp[at++] = b2;
                outp[at++] = b3;
            }
            int pct = at * 100 / outp.Length;
            if (pct != told) { told = pct; progress?.Invoke(pct); }
        }
        return outp;
    }

    // 4:2:2 as the board holds it: Cb Y0 Cr Y1 per sample pair, BT.601,
    // luma 16..235, chroma round 128.
    /// <summary>The 4:2:2 bytes as a Bitmap, on the same path a fetched
    /// card takes. Decodes the bytes the upload sent, not the original
    /// picture: this and ImageReader round differently, and this keeps
    /// an uploaded photo pixel for pixel what a fetch would produce.
    /// </summary>
    public static Bitmap Decode(byte[] raw) => ToBitmap(raw);

    private static Bitmap ToBitmap(byte[] raw)
    {
        var bmp = new Bitmap(W, H, PixelFormat.Format24bppRgb);
        var rect = new Rectangle(0, 0, W, H);
        BitmapData data = bmp.LockBits(rect, ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
        try
        {
            var line = new byte[data.Stride];
            for (int y = 0; y < H; y++)
            {
                int src = y * W * 2;
                for (int x = 0; x < W; x += 2)
                {
                    int cb = raw[src] - 128;
                    int y0 = raw[src + 1] - 16;
                    int cr = raw[src + 2] - 128;
                    int y1 = raw[src + 3] - 16;
                    src += 4;
                    Put(line, x * 3, y0, cb, cr);
                    Put(line, (x + 1) * 3, y1, cb, cr);
                }
                Marshal.Copy(line, 0, data.Scan0 + y * data.Stride, data.Stride);
            }
        }
        finally { bmp.UnlockBits(data); }
        return bmp;
    }

    private static void Put(byte[] line, int at, int y, int cb, int cr)
    {
        int c = 298 * y;
        line[at + 2] = Clip((c + 409 * cr + 128) >> 8);            // red
        line[at + 1] = Clip((c - 100 * cb - 208 * cr + 128) >> 8);  // green
        line[at + 0] = Clip((c + 516 * cb + 128) >> 8);            // blue
    }

    private static byte Clip(int v) => (byte)(v < 0 ? 0 : v > 255 ? 255 : v);


}

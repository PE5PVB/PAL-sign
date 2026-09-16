// One of the two faces of the board, fetched from it, so the preview can
// draw the date and time in the same shape the board would, not a
// Windows font's imitation.
//
// Layout matches textPrepare()/textDrawRow() in the firmware: digits all
// get the width of the widest so a running clock does not jitter, the
// spacing shrinks when the text will not fit, and centring is on the
// real ink rather than the cell height.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Threading;

namespace PalSign;

public class BoardFont
{
    public int Rows, Sx = 1, Sy = 1, Digits, Gap;

    // Bit 31 is the leftmost column, one word to a row: the same shape the
    // firmware keeps them in.
    private readonly Dictionary<char, (int cols, uint[] rows)> _glyph = new();

    public bool Ready => Rows > 0 && _glyph.Count > 0;

    public void Add(char c, int cols, uint[] rows) => _glyph[c] = (cols, rows);

    private (int cols, uint[] rows)? Find(char c)
    {
        if (_glyph.TryGetValue(c, out var g)) return g;
        // Only the PM8546 has lower case; the other folds up, as the
        // firmware does.
        if (c >= 'a' && c <= 'z' && _glyph.TryGetValue((char)(c - 'a' + 'A'), out var up)) return up;
        return null;
    }

    private int WidthOf(char c, (int cols, uint[] rows) g) =>
        (c >= '0' && c <= '9' ? Digits : g.cols) * Sx;

    /// <summary>Draws the text centred in the box, in picture coordinates,
    /// on to a preview rectangle that stands for the whole 720x576.
    /// </summary>
    public void DrawIn(Graphics g, Rectangle view, int x0, int x1, int y0, int y1, string text,
                       Color colour)
    {
        if (!Ready || x1 <= x0 || y1 <= y0) return;

        var use = new List<(char c, int cols, uint[] rows)>();
        foreach (char c in text)
        {
            var f = Find(c);
            if (f != null) use.Add((c, f.Value.cols, f.Value.rows));
        }
        if (use.Count == 0) return;

        int total = 0, top = Rows, bottom = -1;
        foreach (var u in use)
        {
            total += WidthOf(u.c, (u.cols, u.rows));
            for (int r = 0; r < Rows; r++)
            {
                if (u.rows[r] == 0) continue;
                if (r < top) top = r;
                if (r > bottom) bottom = r;
            }
        }
        if (bottom < 0) return;

        // Shrinks when it will not fit, as the firmware does, rather
        // than letting the text run over the edge of the box.
        int gap = Gap;
        int room = x1 - x0;
        while (gap > 0 && total + (use.Count - 1) * gap > room) gap--;

        int penX = x0 + (room - (total + (use.Count - 1) * gap)) / 2;
        // Centred on the ink and not on the cell: the cell also carries
        // room for descenders that most text never uses.
        int penY = y0 + ((y1 - y0) - (bottom - top + 1) * Sy) / 2 - top * Sy;

        float sx = view.Width / (float)CardPreview.ScreenW;
        float sy = view.Height / (float)CardPreview.ScreenH;
        using var brush = new SolidBrush(colour);

        foreach (var u in use)
        {
            for (int r = 0; r < Rows; r++)
            {
                uint bits = u.rows[r];
                if (bits == 0) continue;
                int col = 0;
                while (bits != 0)
                {
                    if ((bits & 0x80000000u) != 0)
                    {
                        int start = col;
                        while ((bits & 0x80000000u) != 0) { bits <<= 1; col++; }
                        var rect = new RectangleF(
                            view.X + (penX + start * Sx) * sx, view.Y + (penY + r * Sy) * sy,
                            (col - start) * Sx * sx, Sy * sy);
                        // At least a pixel: the preview is smaller than the
                        // picture, and a stroke that rounds away leaves a
                        // letter full of holes.
                        g.FillRectangle(brush, rect.X, rect.Y, Math.Max(1f, rect.Width),
                                        Math.Max(1f, rect.Height));
                    }
                    else { bits <<= 1; col++; }
                }
            }
            penX += WidthOf(u.c, (u.cols, u.rows)) + gap;
        }
    }

    // --- off the board -----------------------------------------------------

    /// <summary>Fetches one face. Cheap: about seven kilobytes of text.
    /// </summary>
    public static BoardFont Fetch(string portName, int index, CancellationToken stop)
    {
        var port = new SerialPort(portName, 115200)
        {
            ReadTimeout = 5000, WriteTimeout = 5000, NewLine = "\n",
            DtrEnable = true, RtsEnable = true, ReadBufferSize = 1 << 16,
        };
        port.Open();
        try
        {
            port.DiscardInBuffer();
            Trace.Say($"reading face {index}: sending c");
            port.Write("c");
            WaitFor(port, "[font] ready", stop);
            // Counted from one over the port (readNumber() on the board
            // takes 1..n), while faces are numbered from zero here.
            Trace.Line("send", (index + 1).ToString(CultureInfo.InvariantCulture) + "\n");
            port.Write((index + 1).ToString(CultureInfo.InvariantCulture) + "\n");

            var f = new BoardFont();
            TakeHead(f, WaitFor(port, "[font] " + index, stop));
            if (f.Rows <= 0)
                throw new UploadError("the board did not describe the face");
            var end = DateTime.UtcNow.AddSeconds(20);
            while (DateTime.UtcNow < end)
            {
                string line = ReadLine(port, stop);
                if (line.StartsWith("[font] end", StringComparison.Ordinal)) return f;
                if (line.StartsWith("[glyph] ", StringComparison.Ordinal)) TakeGlyph(f, line[8..]);
            }
            throw new UploadError("the board did not finish the face");
        }
        finally { try { port.Close(); } catch { /* allowed to fail */ } }
    }

    // "[font] 0 rows=30 sx=1 sy=1 digits=14 gap=4 count=55 PM5544"
    private static void TakeHead(BoardFont f, string line)
    {
        foreach (string part in line.Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            // A part without '=' is the face's name at the end of the
            // line; nothing here needs it, the shape is what counts.
            int eq = part.IndexOf('=');
            if (eq <= 0) continue;
            if (!int.TryParse(part[(eq + 1)..], out int v)) continue;
            switch (part[..eq])
            {
                case "rows": f.Rows = v; break;
                case "sx": f.Sx = Math.Max(1, v); break;
                case "sy": f.Sy = Math.Max(1, v); break;
                case "digits": f.Digits = v; break;
                case "gap": f.Gap = v; break;
            }
        }
    }

    // "<character as a number> <columns> <one hex word a row>"
    private static void TakeGlyph(BoardFont f, string rest)
    {
        string[] p = rest.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (p.Length < 2 + f.Rows) return;
        if (!int.TryParse(p[0], out int code) || !int.TryParse(p[1], out int cols)) return;
        var rows = new uint[f.Rows];
        for (int i = 0; i < f.Rows; i++)
            if (!uint.TryParse(p[2 + i], NumberStyles.HexNumber, CultureInfo.InvariantCulture,
                               out rows[i])) return;
        f.Add((char)code, cols, rows);
    }


    private static string ReadLine(SerialPort port, CancellationToken stop)
    {
        var line = new StringBuilder();
        var end = DateTime.UtcNow.AddSeconds(20);
        while (DateTime.UtcNow < end)
        {
            stop.ThrowIfCancellationRequested();
            int c;
            try { c = port.ReadByte(); }
            catch (TimeoutException) { continue; }
            catch (IOException e) { throw new UploadError("connection lost: " + e.Message); }
            if (c < 0) continue;
            if (c == '\n')
            {
                string got = line.ToString();
                // A face is 55 lines of hex; only what is not a glyph is
                // worth a line in the log.
                if (!got.StartsWith("[glyph] ", StringComparison.Ordinal)) Trace.Line("recv", got);
                return got;
            }
            if (c != '\r') line.Append((char)c);
        }
        throw new UploadError("the board went quiet");
    }

    private static string WaitFor(SerialPort port, string start, CancellationToken stop)
    {
        var end = DateTime.UtcNow.AddSeconds(20);
        while (DateTime.UtcNow < end)
        {
            string r = ReadLine(port, stop);
            // Checked before the prefix match: "[font] 0 does not exist"
            // also starts with "[font] ".
            string? bad = BoardWords.Refusal(r);
            if (bad != null) throw new UploadError(bad);
            if (r.StartsWith(start, StringComparison.Ordinal)) return r;
        }
        throw new UploadError($"no reply to \"{start}\"");
    }
}

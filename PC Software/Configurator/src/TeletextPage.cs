// Reads a teletext page made elsewhere, so block graphics (mosaics) can be
// drawn in a real teletext editor and brought over. Every reader returns
// the page as rows of 40 cells, one char per cell, 0x00..0x7F, control
// codes included; row 0 is the header row, unused here.
//
// The formats, as the editors themselves write and read them:
//   edit.tf / zxnet link  "#<hex digit>:<data>[:KEY=value]...", data in
//                         base64url (RFC 4648 section 5), 7 bits a cell,
//                         most significant first, row by row, 40 cells a
//                         row: 1167 characters for 25 rows, 1120 for 24.
//                         Older links: 1920 hex digits, 24 rows.
//   .tti / .ttix (MRG)    "OL,<row>,<text>" lines; a code below 0x20 is
//                         sent with bit 7 set, or as ESC (0x1B) followed by
//                         the code plus 0x40. Only the first subpage.
//   .bin                  25 (or 24) rows of 40 raw bytes; a BBC display
//                         RAM dump (bit 7 set) reads the same way.
//   .t42                  42 byte packets: two Hamming 8/4 address bytes
//                         (magazine, row) and 40 bytes with odd parity.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace PalSign;

public static class TeletextPage
{
    public const int Cols = 40;
    public const int Rows = 25;

    private const string Base64Url =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    /// <summary>A page from a file, by its extension. Null when the file
    /// is not a page in a format this reads.</summary>
    public static string[]? FromFile(string path)
    {
        byte[] data = File.ReadAllBytes(path);
        switch (Path.GetExtension(path).ToLowerInvariant())
        {
            case ".tti":
            case ".ttix":
                return FromTti(data);
            case ".t42":
                return FromT42(data);
            case ".bin":
                return FromBin(data);
            default:
                // A text file holding a link, as some editors save one.
                return FromLink(Encoding.Latin1.GetString(data));
        }
    }

    /// <summary>An edit.tf or zxnet editor link, or only the part after
    /// the '#'. Null when it holds no page.</summary>
    public static string[]? FromLink(string link)
    {
        string s = link.Trim();
        int hash = s.IndexOf('#');
        if (hash >= 0) s = s[(hash + 1)..];
        string[] parts = s.Split(':');
        string data = parts.Length >= 2 ? parts[1] : parts[0];
        data = data.TrimEnd('=');

        if (data.Length == 1920) return FromHex(data);
        int rows = data.Length == 1167 ? 25 : data.Length == 1120 ? 24 : 0;
        if (rows == 0) return null;

        var cells = new char[Rows, Cols];
        Blank(cells);
        int bit = 0, code = 0;
        foreach (char ch in data)
        {
            int v = Base64Url.IndexOf(ch);
            if (v < 0) return null;
            for (int b = 5; b >= 0; b--)
            {
                code = (code << 1) | ((v >> b) & 1);
                bit++;
                if (bit % 7 != 0) continue;
                int n = bit / 7 - 1;
                int r = n / Cols, c = n % Cols;
                if (r < rows) cells[r, c] = (char)code;
                code = 0;
            }
        }
        return ToRows(cells);
    }

    // The older edit.tf form: two hex digits a cell, 24 rows.
    private static string[]? FromHex(string data)
    {
        var cells = new char[Rows, Cols];
        Blank(cells);
        for (int n = 0; n < 24 * Cols; n++)
        {
            if (!int.TryParse(data.AsSpan(2 * n, 2), System.Globalization.NumberStyles.HexNumber,
                              System.Globalization.CultureInfo.InvariantCulture, out int v))
                return null;
            cells[n / Cols, n % Cols] = (char)(v % 128);
        }
        return ToRows(cells);
    }

    private static string[]? FromTti(byte[] data)
    {
        var cells = new char[Rows, Cols];
        Blank(cells);
        bool any = false;
        int pages = 0;
        foreach (string raw in Encoding.Latin1.GetString(data).Split('\n'))
        {
            string line = raw.TrimEnd('\r');
            if (line.StartsWith("PN,", StringComparison.Ordinal) && ++pages > 1) break;   // next subpage
            if (!line.StartsWith("OL,", StringComparison.Ordinal)) continue;
            int comma = line.IndexOf(',', 3);
            if (comma < 0 || !int.TryParse(line.AsSpan(3, comma - 3), out int row)) continue;
            if (row < 0 || row >= Rows) continue;   // 26..29 carry enhancement data, not text
            string text = line[(comma + 1)..];
            int col = 0;
            for (int i = 0; i < text.Length && col < Cols; i++)
            {
                int b = text[i];
                if ((b & 0x80) != 0) b &= 0x7F;
                else if (b == 0x1B && i + 1 < text.Length) b = text[++i] & 0x3F;
                else if (b == 0x0D) b = 0x20;   // a stray carriage return, not double height
                cells[row, col++] = (char)b;
            }
            any = true;
        }
        return any ? ToRows(cells) : null;
    }

    private static string[]? FromBin(byte[] data)
    {
        int rows = data.Length >= Rows * Cols ? Rows : data.Length >= 24 * Cols ? 24 : 0;
        if (rows == 0) return null;
        var cells = new char[Rows, Cols];
        Blank(cells);
        for (int n = 0; n < rows * Cols; n++) cells[n / Cols, n % Cols] = (char)(data[n] & 0x7F);
        return ToRows(cells);
    }

    // Data bits of a Hamming 8/4 byte: bits 2, 4, 6 and 8 counted from 1
    // at the least significant end (ETS 300 706 section 8.2).
    private static int Hamming84(byte b) =>
        ((b >> 1) & 1) | (((b >> 3) & 1) << 1) | (((b >> 5) & 1) << 2) | (((b >> 7) & 1) << 3);

    private static string[]? FromT42(byte[] data)
    {
        var cells = new char[Rows, Cols];
        Blank(cells);
        bool any = false, header = false;
        for (int p = 0; p + 42 <= data.Length; p += 42)
        {
            int n0 = Hamming84(data[p]), n1 = Hamming84(data[p + 1]);
            int row = (n0 >> 3) | (n1 << 1);
            if (row == 0)
            {
                if (header) break;   // the next page starts
                header = true;
                continue;
            }
            if (row >= Rows) continue;
            for (int c = 0; c < Cols; c++) cells[row, c] = (char)(data[p + 2 + c] & 0x7F);
            any = true;
        }
        return any ? ToRows(cells) : null;
    }

    private static void Blank(char[,] cells)
    {
        for (int r = 0; r < Rows; r++)
            for (int c = 0; c < Cols; c++) cells[r, c] = ' ';
    }

    private static string[] ToRows(char[,] cells)
    {
        var rows = new string[Rows];
        var sb = new StringBuilder(Cols);
        for (int r = 0; r < Rows; r++)
        {
            sb.Clear();
            for (int c = 0; c < Cols; c++) sb.Append(cells[r, c]);
            rows[r] = sb.ToString();
        }
        return rows;
    }

    /// <summary>Whether a row needs more than the editor's own text and
    /// alpha colours: block graphics or any other control code.</summary>
    public static bool IsGraphics(string row)
    {
        foreach (char ch in row)
            if (!((ch >= 1 && ch <= 7) || (ch >= 0x20 && ch <= 0x7E))) return true;
        return false;
    }
}

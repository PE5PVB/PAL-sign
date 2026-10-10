// Draws teletext rows the way a Level 1 decoder shows them, for the
// editor's preview: colours, block graphics, hold mosaics, backgrounds,
// double height. The rules are ETS 300 706 section 12.2, table 26:
//   - an attribute shows as a space (or the held mosaic, in hold mode);
//     "Set-At" codes act on their own cell, "Set-After" codes from the
//     next one on;
//   - a mosaic character (columns 2, 3, 6, 7) lights its 2 x 3 blocks
//     from bits 0x01 0x02 / 0x04 0x08 / 0x10 0x40, top left first;
//     codes 0x40..0x5F stay letters even in mosaic mode;
//   - the held mosaic is the last one with bit 0x20 on the row, reset at
//     the row start and on a change of alpha/mosaic mode or of size, and
//     kept in the contiguous/separated form it was drawn in;
//   - double height stretches a character into the row below, whose own
//     contents are then not shown.
// Letters come from the PC's monospace font in plain ASCII, so the few
// national positions (# for a pound sign and the like) may differ from
// a television's; flash shows steady, concealed text shows as spaces,
// boxes and double width are drawn at normal width.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;

namespace PalSign;

public static class TeletextRender
{
    public const int CellW = 12, CellH = 20;

    private static readonly Color[] Colours =
    {
        Color.Black, Color.Red, Color.FromArgb(0, 255, 0), Color.Yellow,
        Color.Blue, Color.Magenta, Color.Cyan, Color.White,
    };

    public static Bitmap Render(string[] rows)
    {
        var bmp = new Bitmap(TeletextPage.Cols * CellW, Math.Max(1, rows.Length) * CellH);
        using var g = Graphics.FromImage(bmp);
        g.Clear(Color.Black);
        g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
        g.SmoothingMode = SmoothingMode.None;
        using var font = new Font(FontFamily.GenericMonospace, 13f, GraphicsUnit.Pixel);
        using var tall = new Font(FontFamily.GenericMonospace, 26f, GraphicsUnit.Pixel);
        var fmt = new StringFormat(StringFormat.GenericTypographic)
            { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };

        bool skipNext = false;
        for (int r = 0; r < rows.Length; r++)
        {
            if (skipNext) { skipNext = false; continue; }
            string row = (rows[r] ?? "").PadRight(TeletextPage.Cols);
            int fg = 7, bg = 0;
            bool mosaic = false, separated = false, hold = false, conceal = false, dbl = false;
            char held = ' ';
            bool heldSeparated = false;
            bool rowHasDouble = false;

            for (int c = 0; c < TeletextPage.Cols; c++)
            {
                int code = row[c] & 0x7F;
                // Set-At, before this cell is drawn.
                switch (code)
                {
                    case 0x09: break;                         // steady
                    case 0x0C:                                // normal size
                        if (dbl) { held = ' '; }
                        dbl = false; break;
                    case 0x18: conceal = true; break;
                    case 0x19: separated = false; break;
                    case 0x1A: separated = true; break;
                    case 0x1C: bg = 0; break;
                    case 0x1D: bg = fg; break;
                    case 0x1E: hold = true; break;
                }

                // What this cell shows.
                char shown = ' ';
                bool shownMosaic = false, shownSeparated = separated;
                if (code < 0x20)
                {
                    if (hold && mosaic) { shown = held; shownMosaic = true; shownSeparated = heldSeparated; }
                }
                else if (mosaic && (code & 0x20) != 0)
                {
                    shown = (char)code;
                    shownMosaic = true;
                    held = (char)code;
                    heldSeparated = separated;
                }
                else
                {
                    shown = (char)code;
                }
                if (conceal) { shown = ' '; shownMosaic = false; }

                var cell = new Rectangle(c * CellW, r * CellH, CellW, dbl ? 2 * CellH : CellH);
                using (var b = new SolidBrush(Colours[bg])) g.FillRectangle(b, cell);
                if (shownMosaic) DrawMosaic(g, cell, shown, Colours[fg], shownSeparated);
                else if (shown != ' ')
                {
                    using var b = new SolidBrush(Colours[fg]);
                    g.DrawString(shown == '\x7F' ? "■" : shown.ToString(), dbl ? tall : font, b, cell, fmt);
                }
                if (dbl) rowHasDouble = true;

                // Set-After, from the next cell on.
                if (code <= 0x07) { fg = code; if (mosaic) held = ' '; mosaic = false; conceal = false; }
                else if (code >= 0x10 && code <= 0x17) { fg = code - 0x10; if (!mosaic) held = ' '; mosaic = true; conceal = false; }
                else if (code == 0x0D) { if (!dbl) held = ' '; dbl = true; }
                else if (code == 0x1F) hold = false;
            }
            // The row below a double-height row only keeps its background.
            if (rowHasDouble && r + 1 < rows.Length) skipNext = true;
        }
        return bmp;
    }

    // The 2 x 3 blocks of a mosaic character in the cell.
    private static void DrawMosaic(Graphics g, Rectangle cell, char ch, Color colour, bool separated)
    {
        int code = ch;
        int[] xs = { cell.X, cell.X + cell.Width / 2, cell.Right };
        int h = cell.Height;
        int[] ys = { cell.Y, cell.Y + h / 3, cell.Y + 2 * h / 3, cell.Bottom };
        int[] bits = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x40 };
        using var b = new SolidBrush(colour);
        for (int i = 0; i < 6; i++)
        {
            if ((code & bits[i]) == 0) continue;
            int col = i % 2, row = i / 2;
            var rc = Rectangle.FromLTRB(xs[col], ys[row], xs[col + 1], ys[row + 1]);
            if (separated) rc = Rectangle.FromLTRB(rc.Left + 2, rc.Top, rc.Right, rc.Bottom - 2);
            g.FillRectangle(b, rc);
        }
    }
}

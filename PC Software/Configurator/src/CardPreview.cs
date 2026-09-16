// The preview of one card: the picture fetched off the board, with
// everything the settings do drawn over it to scale -- letterbox bars,
// ticker band, clock, moving line.
//
// Without a fetched picture, a flat field stands in; no drawn likeness
// of a card, deliberately, since that would be a second implementation
// of seventeen cards. The one exception is the colour bar card: the
// encoder generates that one (advSetColourBars), so the board's
// framebuffer is black there and no fetch can ever show it; its eight
// bars are drawn here instead.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Linq;

namespace PalSign;

public static class CardPreview
{
    public const int ScreenW = 720, ScreenH = 576;

    /// <summary>RRGGBB from a board dump to a Color; anything unreadable
    /// falls back on white, the board's factory setting.</summary>
    public static Color ColorOf(string hex)
    {
        if (hex.Length == 6 &&
            int.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int v))
            return Color.FromArgb((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        return Color.White;
    }

    // The eight bars of BT.471 at 75 per cent, white at 100. The same
    // eight the board draws; see the EBU chapter in the workbook.
    private static readonly Color[] Bars =
    {
        Color.FromArgb(255, 255, 255), Color.FromArgb(191, 191, 0),
        Color.FromArgb(0, 191, 191), Color.FromArgb(0, 191, 0),
        Color.FromArgb(191, 0, 191), Color.FromArgb(191, 0, 0),
        Color.FromArgb(0, 0, 191), Color.FromArgb(0, 0, 0),
    };

    /// <summary>The band the active picture lands in when a letterbox is
    /// on: the sub-rectangle of r that holds `activeLines` of the 576.
    /// Overlays that live INSIDE the picture (the clock, the moving
    /// line) draw against this band, so they land where the squeezed
    /// picture puts their boxes; the ticker stays on the full rectangle,
    /// because on the board it sits outside the aspect ratio too.</summary>
    public static Rectangle ActiveBand(Rectangle r, int activeLines, bool atTop)
    {
        if (activeLines >= ScreenH) return r;
        int bar = atTop ? 0 : ((ScreenH - activeLines) / 2) & ~1;
        int top = r.Y + r.Height * bar / ScreenH;
        int bottom = r.Y + r.Height * (bar + activeLines) / ScreenH;
        return new Rectangle(r.X, top, r.Width, bottom - top);
    }

    /// <summary>Draws the card into the box, keeping 720x576 in shape,
    /// and returns where the picture ended up so a caller can put the
    /// ticker band on it. The picture off the board is the BARE card:
    /// the letterbox and the squeeze are applied here, which is what
    /// lets a WSS change cost no fetch.</summary>
    public static Rectangle Draw(Graphics g, Rectangle box, string name, Image? photo,
                                 int activeLines = ScreenH, bool atTop = false)
    {
        float scale = Math.Min(box.Width / (float)ScreenW, box.Height / (float)ScreenH);
        int w = (int)(ScreenW * scale), h = (int)(ScreenH * scale);
        var r = new Rectangle(box.X, box.Y + (box.Height - h) / 2, w, h);
        if (w < 20 || h < 16) return r;

        using var old = g.Clip;   // a getter that allocates a Region every time
        g.SetClip(r);
        try
        {
            // The colour bar card first, even when a picture was
            // fetched: the board's framebuffer is black there (the
            // encoder makes the bars), so the fetch shows the one thing
            // this card is not.
            if (name.ToUpperInvariant().Contains("COLORBAR"))
            {
                ColourBars(g, r);
                return r;
            }
            if (photo != null)
            {
                if (activeLines < ScreenH)
                {
                    g.FillRectangle(Brushes.Black, r);
                    g.DrawImage(photo, ActiveBand(r, activeLines, atTop));
                }
                else g.DrawImage(photo, r);
                return r;
            }
            // The stand-in until the fetch has run; see the top of this
            // file for why it is a flat field and not a likeness.
            bool empty = name.ToUpperInvariant().Contains("EMPTY");
            Plain(g, r, Color.FromArgb(52, 52, 52),
                  empty ? UiText.PreviewEmptySlot : UiText.PreviewNoPicture);
        }
        finally { g.Clip = old; }
        return r;
    }

    private static void Plain(Graphics g, Rectangle r, Color c, string label)
    {
        using var b = new SolidBrush(c);
        g.FillRectangle(b, r);
        using var ink = new SolidBrush(Color.FromArgb(210, 210, 210));
        using var f = new Font(FontFamily.GenericSansSerif, Math.Max(6f, r.Height / 14f));
        var size = g.MeasureString(label, f);
        g.DrawString(label, f, ink, r.X + (r.Width - size.Width) / 2,
                     r.Y + (r.Height - size.Height) / 2);
    }

    private static void ColourBars(Graphics g, Rectangle r)
    {
        for (int i = 0; i < Bars.Length; i++)
        {
            using var b = new SolidBrush(Bars[i]);
            g.FillRectangle(b, r.X + r.Width * i / Bars.Length, r.Y,
                            r.Width / Bars.Length + 1, r.Height);
        }
    }

    // --- what the settings do, and this part IS to scale -------------------

    /// <summary>The black bars a letterboxed aspect ratio puts on. The
    /// number of active lines per mode is the one from aspect.cpp.</summary>
    public static void Letterbox(Graphics g, Rectangle r, int activeLines, bool atTop)
    {
        if (activeLines >= ScreenH) return;
        int bar = atTop ? 0 : ((ScreenH - activeLines) / 2) & ~1;
        int top = r.Y + r.Height * bar / ScreenH;
        int bottom = r.Y + r.Height * (bar + activeLines) / ScreenH;
        g.FillRectangle(Brushes.Black, r.X, r.Y, r.Width, top - r.Y);
        g.FillRectangle(Brushes.Black, r.X, bottom, r.Width, r.Bottom - bottom);
    }

    /// <summary>The date and the time, drawn in the boxes the board says
    /// the card has. They are NOT in the picture that comes off the board:
    /// they change every second, so a copy with them in it would be wrong
    /// within the second. The black boxes themselves are in it, because
    /// those belong to the card.
    ///
    /// The letters are not the ones the board uses. It has the PM5544 and
    /// the PM8546 and this has neither, so what is drawn here is the right
    /// text in the right place and not the right shape.</summary>
    public static void Clock(Graphics g, Rectangle r, CardBoxes b, DateTime now, int dateFormat,
                             BoardFont? face, Color ink)
    {
        if (!b.HasClock) return;
        string time = now.ToString("HH:mm:ss", CultureInfo.InvariantCulture);
        string date = dateFormat switch
        {
            1 => now.ToString("dd-MM-yyyy", CultureInfo.InvariantCulture),
            2 => now.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture),
            _ => now.ToString("dd-MM-yy", CultureInfo.InvariantCulture),
        };
        // The board's own letters, once fetched; the Windows font below
        // is only a fallback for before that.
        if (face != null && face.Ready)
        {
            face.DrawIn(g, r, b.Rx0, b.Rx1, b.Y0, b.Y1, time, ink);
            if (b.HasDate) face.DrawIn(g, r, b.Lx0, b.Lx1, b.Y0, b.Y1, date, ink);
            return;
        }
        InBox(g, r, b.Rx0, b.Rx1, b.Y0, b.Y1, time);
        if (b.HasDate) InBox(g, r, b.Lx0, b.Lx1, b.Y0, b.Y1, date);
    }

    private static void InBox(Graphics g, Rectangle r, int x0, int x1, int y0, int y1,
                              string text)
    {
        if (x1 <= x0 || y1 <= y0) return;
        var box = new RectangleF(
            r.X + r.Width * x0 / (float)ScreenW, r.Y + r.Height * y0 / (float)ScreenH,
            r.Width * (x1 - x0) / (float)ScreenW, r.Height * (y1 - y0) / (float)ScreenH);
        if (box.Height < 4 || box.Width < 8) return;

        // Made to fit the box rather than set to a size: the boxes are
        // 148 x 40 on the card and the preview can be any size at all.
        float size = box.Height * 0.72f;
        using var f = new Font(FontFamily.GenericSansSerif, size, FontStyle.Bold,
                               GraphicsUnit.Pixel);
        SizeF got = g.MeasureString(text, f);
        if (got.Width > box.Width)
        {
            using var thin = new Font(FontFamily.GenericSansSerif,
                                      size * box.Width / got.Width, FontStyle.Bold,
                                      GraphicsUnit.Pixel);
            got = g.MeasureString(text, thin);
            g.DrawString(text, thin, Brushes.White, box.X + (box.Width - got.Width) / 2,
                         box.Y + (box.Height - got.Height) / 2);
            return;
        }
        g.DrawString(text, f, Brushes.White, box.X + (box.Width - got.Width) / 2,
                     box.Y + (box.Height - got.Height) / 2);
    }

    /// <summary>The moving line, animated the way the board sweeps it:
    /// corner to corner in one second each way, resting one field in each
    /// corner. The stripe is left out of the picture the board sends
    /// because it moves, so the preview draws it itself. t is a running
    /// clock in seconds; the box comes with the card dump.</summary>
    public static void MovingLine(Graphics g, Rectangle r, CardBoxes bx, double t)
    {
        if (!bx.HasLine) return;
        // The board steps per field, 50 a second: phases 0..49 sweep
        // right, 50..99 mirror back, the corner shared between the legs.
        int ph = (int)(t * 50.0) % 100;
        int f = ph < 50 ? ph : 99 - ph;
        const int lineW = 4;
        int travel = Math.Max(0, bx.Mx1 - lineW - bx.Mx0);
        double x = bx.Mx0 + travel * f / 49.0;
        int px = r.X + (int)(r.Width * x / ScreenW);
        int pw = Math.Max(1, r.Width * lineW / ScreenW);
        int py = r.Y + r.Height * bx.My0 / ScreenH;
        int phh = Math.Max(1, r.Height * (bx.My1 - bx.My0) / ScreenH);
        g.FillRectangle(Brushes.White, px, py, pw, phh);
    }

    /// <summary>The ticker band, drawn where it really lands. offset is
    /// how far the message has run, in samples of the 720, so that the
    /// text can be made to walk the same way it does on the board.
    /// </summary>
    public static void Ticker(Graphics g, Rectangle r, int mode, int y, int band, string text,
                              int offset, Color ink)
    {
        if (mode == 0) return;
        int by = r.Y + r.Height * y / ScreenH;
        int bh = Math.Max(2, r.Height * band / ScreenH);

        if (mode == 1)
        {
            g.FillRectangle(Brushes.Black, r.X, by, r.Width, bh);
        }
        else
        {
            // Over the picture there is no bar, only the letters with
            // their ring, so what is behind them stays visible.
            using var shade = new SolidBrush(Color.FromArgb(70, 0, 0, 0));
            g.FillRectangle(shade, r.X, by, r.Width, bh);
        }

        // The message itself, cut off at the edge the way it really is.
        if (bh >= 6 && text.Length > 0)
        {
            // The board letters in capitals only (its faces have no lower
            // case; the fold sits in ticker.cpp), so the preview folds too.
            text = string.Concat(text.Select(BoardText.Fold));
            using var clip = g.Clip;  // idem
            g.SetClip(new Rectangle(r.X, by, r.Width, bh), CombineMode.Intersect);
            using var f = new Font(FontFamily.GenericSansSerif, Math.Max(5f, bh * 0.62f),
                                   FontStyle.Bold, GraphicsUnit.Pixel);
            // The lettering in the chosen colour; the outline stays
            // black, exactly as on the board.
            using var brush = new SolidBrush(ink);
            using var edge = new Pen(Color.Black, 2f);
            // In a using, so a throw between here and the Dispose below
            // cannot leak the path or leave the clip set.
            using var path = new GraphicsPath();
            // Twice, a screen apart, so the message runs off the left and
            // comes back on the right without a gap; the board does the
            // same with a turn that is never shorter than a screen.
            float step = r.Width * offset / (float)ScreenW;
            float x = r.X + 2 - step;
            path.AddString(text, f.FontFamily, (int)FontStyle.Bold, f.Size,
                           new PointF(x, by + bh * 0.15f), StringFormat.GenericTypographic);
            path.AddString(text, f.FontFamily, (int)FontStyle.Bold, f.Size,
                           new PointF(x + r.Width, by + bh * 0.15f),
                           StringFormat.GenericTypographic);
            var mode2 = g.SmoothingMode;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.DrawPath(edge, path);
            g.FillPath(brush, path);
            g.SmoothingMode = mode2;
            g.Clip = clip;
        }

        using var pen = new Pen(Color.FromArgb(0, 140, 255), 1f);
        g.DrawRectangle(pen, r.X, by, r.Width - 1, bh);
    }
}

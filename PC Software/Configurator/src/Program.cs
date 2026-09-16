// Starting point. Also has hidden command line modes:
//
//     PAL-sign.exe --dump <in> <out.bin> [--flicker s] [--fill] [--square]
//
// Converts an image to bare 4:2:2 bytes, no window, no board. Used by
// compare.py to check this program's image conversion against
// Firmware/tools/convert_image.py, byte for byte.
//
//     PAL-sign.exe --algo-version
//
// Prints ImageConvert.AlgoVersion, so compare.py can check it against
// convert_image.py's own ALGO_VERSION before trusting a byte-for-byte
// pass as proof the two are still the same algorithm.
using System;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Windows.Forms;

namespace PalSign;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--algo-version")
        {
            Console.WriteLine(ImageConvert.AlgoVersion);
            return 0;
        }
        if (args.Length > 0 && args[0] == "--dump") return Dump(args);
        if (args.Length > 0 && args[0] == "--layout") return Layout(args);
        if (args.Length > 0 && args[0] == "--preview") return Preview(args);
        if (args.Length > 0 && args[0] == "--screenshot") return Screenshot(args);

        ApplicationConfiguration.Initialize();
        Application.Run(new MainWindow());
        // Only reached on a real window close, not the headless modes above.
        Trace.Clear();
        return 0;
    }

    // Builds the window, lays it out, and reports elements that overlap
    // or fall outside their parent.
    private static int Layout(string[] args)
    {
        string target = args.Length > 1 ? args[1] : "layout.txt";
        var outp = new System.Text.StringBuilder();
        int problems = 0;
        try
        {
            ApplicationConfiguration.Initialize();
            using var win = new MainWindow();

            // CreateControl() alone leaves the tab pages empty; the window
            // has to actually be shown, off screen, for them to build.
            win.StartPosition = FormStartPosition.Manual;
            win.Location = new Point(-32000, -32000);
            win.Show();
            Application.DoEvents();
            win.PerformLayout();
            Application.DoEvents();

            // Walk() only descends into Visible controls, and a TabControl
            // keeps one page visible at a time, so each page is walked in
            // turn as well as the window itself (for the bar along the top).
            var tabs = FindTabs(win);
            if (tabs == null)
            {
                foreach (Control c in win.Controls) Walk(c, outp, ref problems, 0);
            }
            else
            {
                foreach (Control c in win.Controls) Walk(c, outp, ref problems, 0);

                foreach (TabPage page in tabs.TabPages)
                {
                    // Per-card switches are invisible until a card is
                    // selected; shown by hand since there is no board here.
                    tabs.SelectedTab = page;
                    if (page is CardsTab cards)
                    {
                        cards.ShowEveryOption();
                        outp.AppendLine("(the per-card switches are shown for this check)");
                    }
                    page.PerformLayout();
                    Application.DoEvents();
                    Walk(page, outp, ref problems, 0);

                    // And the other extreme: a TableLayoutPanel skips
                    // invisible children when handing out cells, so a
                    // hidden row is exactly where things shift column.
                    if (page is CardsTab hide)
                    {
                        // Every optional row forced on makes the options
                        // taller than any real card, so only width is
                        // checked against the floor here.
                        MeasurePreview(page, outp, ref problems, widthOnly: true);
                        hide.HideEveryOption();
                        page.PerformLayout();
                        Application.DoEvents();
                        outp.AppendLine("(and now with every optional row left out)");
                        Walk(page, outp, ref problems, 0);
                        MeasurePreview(page, outp, ref problems, widthOnly: false);
                    }
                }
            }
            win.Close();
        }
        catch (Exception e)
        {
            outp.AppendLine("could not build the window: " + e);
            problems++;
        }
        outp.AppendLine(problems == 0 ? "NO overlap and nothing outside its parent."
                                      : $"{problems} problem(s) found.");
        File.WriteAllText(target, outp.ToString());
        return problems == 0 ? 0 : 1;
    }

    private static TabControl? FindTabs(Control c)
    {
        if (c is TabControl tc) return tc;
        foreach (Control child in c.Controls)
        {
            var found = FindTabs(child);
            if (found != null) return found;
        }
        return null;
    }

    private static void Walk(Control parent, System.Text.StringBuilder outp, ref int problems, int depth)
    {
        string indent = new string(' ', depth * 2);
        outp.AppendLine($"{indent}{parent.GetType().Name} \"{Shorten(parent.Text)}\" {parent.Bounds}");

        var visible = new System.Collections.Generic.List<Control>();
        foreach (Control child in parent.Controls) if (child.Visible) visible.Add(child);

        // A scrolling parent may be shorter than its contents; overlap
        // inside it is still an error.
        bool scrolls = parent is ScrollableControl sc && sc.AutoScroll;
        if (scrolls) outp.AppendLine($"{indent}  (scrolls, so longer than its box is allowed)");

        for (int i = 0; i < visible.Count; i++)
        {
            Control a = visible[i];
            Rectangle cl = parent.ClientRectangle;
            if (!scrolls &&
                (a.Bounds.Right > cl.Right + 1 || a.Bounds.Bottom > cl.Bottom + 1 ||
                 a.Bounds.Left < cl.Left - 1 || a.Bounds.Top < cl.Top - 1))
            {
                outp.AppendLine($"{indent}  ! {a.GetType().Name} \"{Shorten(a.Text)}\" {a.Bounds} " +
                                $"falls outside {parent.GetType().Name} {cl}");
                problems++;
            }
            for (int j = i + 1; j < visible.Count; j++)
            {
                Control b = visible[j];
                // A TabControl's bounds cover the whole tab row, but
                // right of the last tab is dead space; a sibling there
                // is not an overlap.
                if (OnStrip(a, b) || OnStrip(b, a)) continue;
                Rectangle overlap = Rectangle.Intersect(a.Bounds, b.Bounds);
                if (overlap.Width > 1 && overlap.Height > 1)
                {
                    outp.AppendLine($"{indent}  ! {a.GetType().Name} \"{Shorten(a.Text)}\" and " +
                                    $"{b.GetType().Name} \"{Shorten(b.Text)}\" overlap at {overlap}");
                    problems++;
                }
            }
        }
        foreach (Control child in visible) Walk(child, outp, ref problems, depth + 1);
    }

    // The preview panel draws at min(w/720, h/576) of full size; a
    // resulting picture under 400 px wide is a fault.
    private static void MeasurePreview(Control page, System.Text.StringBuilder outp,
                                       ref int problems, bool widthOnly)
    {
        Control? panel = FindByName(page, "SteadyPanel");
        if (panel == null)
        {
            outp.AppendLine("  ! no preview panel found to measure");
            problems++;
            return;
        }
        double scale = Math.Min(panel.ClientSize.Width / 720.0,
                                panel.ClientSize.Height / 576.0);
        int wide = widthOnly ? panel.ClientSize.Width : (int)(720 * scale);
        outp.AppendLine($"(preview panel {panel.ClientSize.Width}x{panel.ClientSize.Height}, " +
                        $"picture {(int)(720 * scale)} wide)");
        if (wide < 400)
        {
            outp.AppendLine($"  ! the preview {(widthOnly ? "panel is" : "draws a picture")} " +
                            $"{wide} wide, the floor is 400");
            problems++;
        }
    }

    private static Control? FindByName(Control root, string typeName)
    {
        if (root.GetType().Name == typeName) return root;
        foreach (Control c in root.Controls)
        {
            Control? hit = FindByName(c, typeName);
            if (hit != null) return hit;
        }
        return null;
    }

    private static bool OnStrip(Control a, Control b) =>
        a is TabControl t && b.Bounds.Bottom <= t.Bounds.Top + t.DisplayRectangle.Top;

    private static string Shorten(string? t) =>
        string.IsNullOrEmpty(t) ? "" : (t.Length > 24 ? t[..24] : t);

    // Draws every card sketch into one picture, to check CardPreview
    // without a board or clicking through the list.
    private static readonly string[] SampleCards =
    {
        "PM5644", "Simple FUBK", "FUBK", "EBU bars red",
        "BBC Card F", "BBC Card G", "TVE Card", "PM5644 16:9",
        "FUBK 16:9", "BBC Card W", "Colorbar", "Indian Head BW",
        "EBU BW", "Pulse & Bar", "sin(x)/x", "Multiburst",
        "ATV Contest", "Custom 1 (uploadable, key u)",
    };

    private static int Preview(string[] args)
    {
        string target = args.Length > 1 ? args[1] : "preview.png";
        const int TileW = 260, TileH = 208, Cols = 6, Pad = 8, Label = 18;
        int rows = (SampleCards.Length + Cols - 1) / Cols;
        using var bmp = new System.Drawing.Bitmap(Cols * (TileW + Pad) + Pad,
                                                  rows * (TileH + Pad + Label) + Pad);
        using (var g = System.Drawing.Graphics.FromImage(bmp))
        using (var font = new System.Drawing.Font(System.Drawing.FontFamily.GenericSansSerif, 8f))
        {
            g.Clear(System.Drawing.Color.FromArgb(240, 240, 240));
            for (int i = 0; i < SampleCards.Length; i++)
            {
                int cx = Pad + (i % Cols) * (TileW + Pad);
                int cy = Pad + (i / Cols) * (TileH + Pad + Label);
                var box = new System.Drawing.Rectangle(cx, cy, TileW, TileH);
                var r = CardPreview.Draw(g, box, SampleCards[i], null);
                // Every third one with a band, and one letterboxed, so
                // both of those can be looked at as well.
                if (i % 3 == 0)
                    CardPreview.Ticker(g, r, 1, 530, 38, "PE5PVB TEST CARD", 0,
                                       System.Drawing.Color.White);
                else if (i % 3 == 1)
                    CardPreview.Ticker(g, r, 2, 300, 38, "PE5PVB TEST CARD", 0,
                                       System.Drawing.Color.White);
                if (i == 5) CardPreview.Letterbox(g, r, 430, false);
                g.DrawRectangle(System.Drawing.Pens.Black, r.X, r.Y, r.Width - 1, r.Height - 1);
                g.DrawString(SampleCards[i], font, System.Drawing.Brushes.Black,
                             cx, cy + TileH + 2);
            }
        }
        bmp.Save(target, System.Drawing.Imaging.ImageFormat.Png);
        Console.WriteLine($"{SampleCards.Length} sketches in {target}");
        return 0;
    }

    // Renders the real window, off screen, to PNG per tab -- for looking
    // at the actual control rendering (not a hand drawn mock) without a
    // display. Dev-only, the same off-screen trick as --layout.
    //
    //     PAL-sign.exe --screenshot <outdir>
    private static int Screenshot(string[] args)
    {
        string dir = args.Length > 1 ? args[1] : ".";
        Directory.CreateDirectory(dir);
        ApplicationConfiguration.Initialize();
        using var win = new MainWindow();
        win.StartPosition = FormStartPosition.Manual;
        win.Location = new Point(-32000, -32000);
        win.Show();
        Application.DoEvents();
        win.PerformLayout();
        Application.DoEvents();

        var tabs = FindTabs(win);
        if (tabs != null)
        {
            foreach (TabPage page in tabs.TabPages)
            {
                tabs.SelectedTab = page;
                if (page is CardsTab cards) cards.ShowEveryOption();
                page.PerformLayout();
                Application.DoEvents();
                Capture(win, Path.Combine(dir, SafeName(page.Text) + ".png"));
            }
        }
        else
        {
            Capture(win, Path.Combine(dir, "window.png"));
        }
        win.Close();
        Console.WriteLine($"screenshots written to {dir}");
        return 0;
    }

    private static void Capture(Form win, string path)
    {
        using var bmp = new Bitmap(win.Width, win.Height);
        win.DrawToBitmap(bmp, new Rectangle(Point.Empty, win.Size));
        bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
    }

    private static string SafeName(string s)
    {
        foreach (char c in Path.GetInvalidFileNameChars()) s = s.Replace(c, '_');
        return s.ToLowerInvariant();
    }

    private static int Dump(string[] args)
    {
        if (args.Length < 3) return 2;
        string src = args[1], target = args[2];
        double flicker = 1.0;
        bool fill = false, square = false;
        for (int i = 3; i < args.Length; i++)
        {
            if (args[i] == "--flicker" && i + 1 < args.Length)
                flicker = double.Parse(args[++i], CultureInfo.InvariantCulture);
            else if (args[i] == "--fill") fill = true;
            else if (args[i] == "--square") square = true;
        }
        try
        {
            double[,,] rgb = ImageReader.Read(src);
            double[,,] canvas = ImageConvert.FitIn(rgb, fill, square);
            File.WriteAllBytes(target, ImageConvert.To422(canvas, flicker));
            return 0;
        }
        catch (Exception e)
        {
            File.WriteAllText(target + ".error.txt", e.ToString());
            return 1;
        }
    }
}

// PCM decoder for the PAL-sign board: captures the composite output
// through a Blackmagic DeckLink card, decodes the audio it carries (the
// board's own Ham PCM card, src/testcards/pcmaudio.cpp, 48 kHz, or the
// EIAJ STC-007 / Sony PCM-F1 format, src/testcards/sonypcm.cpp, 44.1
// kHz) and plays it on a soundcard, with everything the decoders
// measure on screen.
//
// Usage: PcmDecoder.exe [DeckLink name] [soundcard]
// An ASIO route is picked by its listed name or a piece of it, for
// example "3&4" for "ASIO: GIGAPORT eX [ch 3&4]".
// Both optional and only preselect the drop-downs; `soundcard` is an
// index or a (partial, case insensitive) name.
// PcmDecoder.exe --selftest <selftest_vectors.txt> checks the Ham PCM
// decoder against the reference model's vectors and exits;
// --screenshot <file.png> saves the idle window and exits.
using System;
using System.Collections.Generic;
using System.Windows.Forms;
using PcmDecoder;

internal static class Program
{
    // A WinExe has no console; for the command line modes this borrows
    // the one the process was started from, so their output lands in
    // that terminal.
    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern bool AttachConsole(int pid);
    private const int AttachParentProcess = -1;

    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length == 2 && (args[0] == "--selftest" || args[0] == "--screenshot"))
        {
            AttachConsole(AttachParentProcess);
            Console.WriteLine();
        }
        if (args.Length == 2 && args[0] == "--selftest") return SelfTest(args[1]);
        ApplicationConfiguration.Initialize();
        if (args.Length == 2 && args[0] == "--screenshot")
        {
            // The idle window drawn to a file, to look at the layout
            // without a board or a capture card.
            var f = new MainForm("", "");
            f.Show();
            Application.DoEvents();
            using var bmp = new System.Drawing.Bitmap(f.Width, f.Height);
            f.DrawToBitmap(bmp, new System.Drawing.Rectangle(0, 0, f.Width, f.Height));
            bmp.Save(args[1]);
            f.Close();
            return 0;
        }
        Application.Run(new MainForm(args.Length > 0 ? args[0] : "", args.Length > 1 ? args[1] : ""));
        return 0;
    }

    // --selftest: rows from selftest_vectors.txt (written by
    // Firmware/tools/pcm_v3_ref.py: 20 row bytes, then the reference
    // model's verdict) are drawn as pixels and must decode the same.
    private static int SelfTest(string path)
    {
        int fail = 0, count = 0;
        foreach (string line in System.IO.File.ReadLines(path))
        {
            var parts = line.Split(' ');
            if (parts.Length != 2) continue;
            count++;
            var px = RowPixels(parts[0]);
            var dec = new RowDecoder();
            var got = dec.DecodeRow(px, 0, px.Length);
            string verdict;
            if (dec.RowsOk == 0) verdict = "-";
            else
            {
                var s = new List<string>();
                foreach (var pr in got) s.Add($"{pr.Left >> 2}/{pr.Right >> 2}");
                verdict = (dec.RowsCoarse > 0 ? "coarse:" : "full:") + string.Join(",", s);
            }
            if (verdict != parts[1])
            {
                fail++;
                if (fail <= 5) Console.WriteLine($"vector {count}: expected {parts[1]}, got {verdict}");
            }
        }
        Console.WriteLine($"selftest: {count} vectors, {fail} differ from the reference");
        // Speed: the same 600 rows decoded again, warmed up, against the
        // 40 ms a frame of 576 rows allows.
        {
            var rowsPx = new List<byte[]>();
            foreach (string line in System.IO.File.ReadLines(path))
            {
                var parts = line.Split(' ');
                if (parts.Length == 2) rowsPx.Add(RowPixels(parts[0]));
            }
            var dec = new RowDecoder();
            foreach (var px in rowsPx) dec.DecodeRow(px, 0, px.Length);
            var sw = System.Diagnostics.Stopwatch.StartNew();
            foreach (var px in rowsPx) dec.DecodeRow(px, 0, px.Length);
            sw.Stop();
            Console.WriteLine($"selftest speed: {rowsPx.Count} rows in {sw.Elapsed.TotalMilliseconds:0.0} ms " +
                              $"= {sw.Elapsed.TotalMilliseconds * 576 / rowsPx.Count:0.0} ms a frame");
        }
        // Stress: the clean vectors drawn at a bit period slightly off
        // the nominal 696/180 and starting where a capture card puts the
        // 12 px guard, shifted a few pixels either way, as a capture
        // them; the phase tracker must keep every row full.
        // Above 4.00 the 180th bit falls past pixel 719, so 4.01 loses its
        // CRC bit by geometry: the row's edge, not the decoder.
        foreach (double per in new[] { 3.83, 3.85, 3.88, 3.90 })
        foreach (int start in new[] { 4, 12, 20 })
        {
            long ok = 0, coarse = 0, lost = 0, total = 0;
            foreach (string line in System.IO.File.ReadLines(path))
            {
                var parts = line.Split(' ');
                if (parts.Length != 2 || !parts[1].StartsWith("full:")) continue;
                total++;
                var dec = new RowDecoder();
                dec.DecodeRow(RowPixels(parts[0], per, start), 0, 720 * 2);
                if (dec.RowsCoarse > 0) coarse++;
                else if (dec.RowsOk > 0) ok++;
                else lost++;
            }
            Console.WriteLine($"selftest stress period {per:0.00} start {start}: {total} clean rows, {ok} full, {coarse} coarse, {lost} lost");
        }

        // The text channel: selftest_text.txt next to the vectors, the
        // expected text then one cycle of rows.
        string textPath = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(path) ?? "", "selftest_text.txt");
        var textLines = System.IO.File.ReadAllLines(textPath);
        var tdec = new RowDecoder();
        for (int r = 1; r < textLines.Length; r++)
        {
            if (textLines[r].Length < 40) continue;
            tdec.DecodeRow(RowPixels(textLines[r]), 0, 720 * 2);
        }
        bool textOk = tdec.Text == textLines[0];
        Console.WriteLine($"selftest text: expected \"{textLines[0]}\", got \"{tdec.Text}\"");

        // The LQ and Voice rows: selftest_lq.txt (15 row bytes, 696/140 px
        // a bit) and selftest_voice.txt (11 row bytes, 696/108 px a bit),
        // each line the bytes as hex and the verdict with 16 bit values.
        int lowFail = 0;
        foreach (var (file, period, fam) in new[] { ("selftest_lq.txt", 696.0 / 140.0, 1), ("selftest_voice.txt", 696.0 / 108.0, 2),
                                                    ("selftest_narrow.txt", 696.0 / 76.0, 3) })
        {
            string lowPath = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(path) ?? "", file);
            int cnt = 0, bad = 0;
            foreach (string line in System.IO.File.ReadLines(lowPath))
            {
                var parts = line.Split(' ');
                if (parts.Length != 2) continue;
                cnt++;
                var dec = new RowDecoder();
                var got = dec.DecodeRow(RowPixels(parts[0], period, 0, parts[0].Length / 2), 0, 720 * 2);
                string verdict;
                if (dec.RowsOk == 0) verdict = "-";
                else
                {
                    var v = new List<string>();
                    foreach (var pr in got) v.Add($"{pr.Left}/{pr.Right}");
                    verdict = (dec.RowsCoarse > 0 ? "coarse:" : "full:") + string.Join(",", v);
                }
                if (verdict != parts[1] || dec.LastFamily != fam)
                {
                    bad++;
                    if (bad <= 3) Console.WriteLine($"{file} vector {cnt}: expected {parts[1]}, got {verdict} (family {dec.LastFamily})");
                }
            }
            Console.WriteLine($"selftest {file}: {cnt} vectors, {bad} differ from the reference");
            lowFail += bad;
        }
        return fail == 0 && textOk && lowFail == 0 ? 0 : 1;
    }

    // A row of pixels (720, packed 4:2:2) from 20 hex row bytes, bit b
    // spanning pixels round(start + b period) .. round(start + (b + 1) period) - 1.
    private static byte[] RowPixels(string hex, double period = 696.0 / 180.0, int start = 12, int nBytes = 20)
    {
        var bits = new List<int>();
        for (int i = 0; i < 8; i++) bits.Add(i & 1);
        for (int i = 0; i < 4; i++) bits.Add(0);
        void Byte(int b) { for (int i = 7; i >= 0; i--) bits.Add((b >> i) & 1); }
        Byte(0x2E);
        for (int i = 0; i < nBytes; i++) Byte(Convert.ToInt32(hex.Substring(2 * i, 2), 16));
        var px = new byte[720 * 2];
        for (int p = 0; p < 720; p++) px[(p / 2) * 4 + (p % 2 == 0 ? 1 : 3)] = 16;
        for (int b = 0; b < bits.Count; b++)
        {
            int x0 = (int)Math.Round(start + b * period), x1 = (int)Math.Round(start + (b + 1) * period);
            for (int p = Math.Max(0, x0); p < Math.Min(720, x1); p++)
                px[(p / 2) * 4 + (p % 2 == 0 ? 1 : 3)] = (byte)(bits[b] != 0 ? 235 : 16);
        }
        return px;
    }
}

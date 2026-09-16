// Logs every line sent or received over the port, with a timestamp and
// the gap since the line before, so a failed exchange can be read back
// instead of guessed at. Always on, capped at 1 MB (see Trim()).
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;

namespace PalSign;

public static class Trace
{
    private static readonly object Lock = new();
    private static DateTime _last = DateTime.UtcNow;

    public static string Path => System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "PAL-sign", "log.txt");

    public static void Say(string what)
    {
        try
        {
            lock (Lock)
            {
                var now = DateTime.UtcNow;
                double since = (now - _last).TotalMilliseconds;
                _last = now;
                string dir = System.IO.Path.GetDirectoryName(Path) ?? ".";
                Directory.CreateDirectory(dir);
                Trim();
                File.AppendAllText(
                    Path,
                    string.Format(CultureInfo.InvariantCulture, "{0:HH:mm:ss.fff} +{1,7:F1} ms  {2}",
                                  now.ToLocalTime(), since, what) + Environment.NewLine,
                    Encoding.UTF8);
            }
        }
        catch (Exception) { /* a log that gets in the way is worse than none */ }
    }

    /// <summary>What was sent or received, with anything unprintable shown
    /// as a number so a stray byte cannot hide in it.</summary>
    public static void Line(string way, string text) => Say(way + " " + Readable(text));

    private static string Readable(string s)
    {
        var b = new StringBuilder(s.Length + 8);
        b.Append('"');
        foreach (char c in s)
        {
            if (c == '\n') b.Append("\\n");
            else if (c == '\r') b.Append("\\r");
            else if (c < ' ' || c > '~') b.Append("<").Append((int)c).Append('>');
            else b.Append(c);
        }
        b.Append('"');
        return b.ToString();
    }

    private static void Trim()
    {
        try
        {
            var f = new FileInfo(Path);
            if (f.Exists && f.Length > 1_000_000) f.Delete();
        }
        catch (Exception) { /* allowed to fail */ }
    }

    /// <summary>Wipes the log. Called once from Program.cs as the window
    /// closes, so every run starts empty; a crash loses that run's log.</summary>
    public static void Clear()
    {
        try
        {
            var f = new FileInfo(Path);
            if (f.Exists) f.Delete();
        }
        catch (Exception) { /* allowed to fail */ }
    }

    public static void Show()
    {
        try
        {
            if (!File.Exists(Path)) Say("nothing has been logged yet");
            Process.Start(new ProcessStartInfo(Path) { UseShellExecute = true });
        }
        catch (Exception) { /* allowed to fail */ }
    }
}

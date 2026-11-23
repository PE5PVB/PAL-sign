// What this program remembers between runs: the port it last connected
// to. One small text file under the user's local application data,
// next to the card cache; missing or unreadable means nothing is
// remembered, which is what a first run looks like.
using System;
using System.IO;

namespace PalSign;

internal static class Prefs
{
    private static string Folder =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                     "PAL-sign");

    private static string PortFile => Path.Combine(Folder, "lastport.txt");

    /// <summary>The port name of the last successful Connect, or null.</summary>
    public static string? LastPort
    {
        get
        {
            try
            {
                string s = File.ReadAllText(PortFile).Trim();
                return s.Length > 0 ? s : null;
            }
            catch { return null; }
        }
        set
        {
            try
            {
                if (value == null) return;
                Directory.CreateDirectory(Folder);
                File.WriteAllText(PortFile, value);
            }
            catch { /* a preference not kept is no reason to fail the connect */ }
        }
    }
}

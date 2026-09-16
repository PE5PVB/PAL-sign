// The words the board says when something went wrong, in one place.
// Protocol: keep in step with the firmware's own use of "failed",
// "aborted", "too small" and "does not exist".
//
// Only looked for before the first quote on the line: the board echoes a
// photograph's file name back in quotes, and "capture-failed.png" must
// not read as a refusal.
using System;

namespace PalSign;

public static class BoardWords
{
    private static readonly string[] Words =
        { "failed", "aborted", "too small", "does not exist" };

    /// <summary>The refusal in this line, or null when it is not one.
    /// </summary>
    public static string? Refusal(string line)
    {
        int quote = line.IndexOf('"');
        string look = quote >= 0 ? line[..quote] : line;
        foreach (var w in Words)
            if (look.Contains(w, StringComparison.Ordinal)) return line.Trim();
        return null;
    }
}

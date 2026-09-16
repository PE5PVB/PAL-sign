// What can be typed into a box whose text ends up lettered on the
// television. The board owns three faces for that lettering and all
// three carry exactly one character set (CHARS in
// Firmware/tools/make_fonts.py, mirrored by make_contest_font.py for
// the run faces); a character outside it would be drawn as a blank on
// the board, so it is refused here, at the keyboard, with the Windows
// sound that says no. Lower case is folded to upper case while typing,
// because that is what the board does to it anyway: the boxes then show
// what the television will show.
//
// Pasting goes around KeyPress, so the text is checked again whenever
// it changes: folded to upper case, anything unknown dropped, and the
// same sound played when something was.
using System;
using System.Windows.Forms;

namespace PalSign;

internal static class BoardText
{
    // Byte for byte the CHARS of make_fonts.py. The slashed O is the
    // one character above ASCII; the board reads it in UTF-8, Latin-1
    // or CP1252 alike.
    public const string Allowed =
        " !\"&'()+,-./0123456789:;=?ABCDEFGHIJKLMNOPQRSTUVWXYZ_Ø";

    /// <summary>The fold the board applies: lower case up, and the
    /// small slashed o to the capital.</summary>
    public static char Fold(char c)
    {
        if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
        if (c == 'ø') return 'Ø';
        return c;
    }

    /// <summary>Guard one box: fold while typing, refuse what the board
    /// cannot letter, and clean up a paste the same way.</summary>
    public static void Guard(TextBox box)
    {
        box.KeyPress += (_, e) =>
        {
            if (char.IsControl(e.KeyChar)) return;   // backspace and friends
            char c = Fold(e.KeyChar);
            if (Allowed.IndexOf(c) < 0)
            {
                e.Handled = true;
                System.Media.SystemSounds.Beep.Play();
                return;
            }
            e.KeyChar = c;   // the folded character is what goes in
        };
        box.TextChanged += (_, __) =>
        {
            string s = box.Text;
            var kept = new System.Text.StringBuilder(s.Length);
            int caret = box.SelectionStart;
            int lost = 0;
            for (int i = 0; i < s.Length; i++)
            {
                char c = Fold(s[i]);
                if (Allowed.IndexOf(c) >= 0) kept.Append(c);
                else if (i < caret) lost++;
            }
            string clean = kept.ToString();
            if (clean == s) return;
            // Setting Text moves the caret to 0, so it is put back where
            // it was, less what was dropped in front of it.
            box.Text = clean;
            box.SelectionStart = Math.Max(0, Math.Min(clean.Length, caret - lost));
            // Only when the user is in the box: a text fetched from the
            // board lands here too, and cleaning that up is not a
            // mistake anyone made just now.
            if (clean.Length != s.Length && box.Focused)
                System.Media.SystemSounds.Beep.Play();
        };
    }
}

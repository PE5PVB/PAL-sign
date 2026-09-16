// One set of colours and a couple of styling helpers, used everywhere
// instead of leaving controls on the WinForms system defaults. Accent
// is not a new colour invented for this: it is CardPreview.cs's own
// ticker-band outline (Color.FromArgb(0, 140, 255)), the same blue the
// board's own picture already carries, applied to the chrome around it.
using System.Drawing;
using System.Windows.Forms;

namespace PalSign;

public static class Theme
{
    public static readonly Color Ink = Color.FromArgb(28, 37, 40);
    public static readonly Color Paper = Color.FromArgb(243, 245, 246);
    public static readonly Color Panel = Color.White;
    public static readonly Color Line = Color.FromArgb(214, 221, 224);
    public static readonly Color Muted = Color.FromArgb(112, 128, 133);
    public static readonly Color Accent = Color.FromArgb(0, 140, 255);
    public static readonly Color AccentDark = Color.FromArgb(0, 112, 210);
    public static readonly Color AccentDarker = Color.FromArgb(0, 90, 175);
    public static readonly Color AccentInk = Color.White;

    /// <summary>A flat button: filled accent for the one primary action
    /// on a screen, a quiet outline for everything else. FlatAppearance
    /// covers the hover/press shades, since FlatStyle alone leaves the
    /// system's own grey.</summary>
    public static void StyleButton(Button b, bool primary = false)
    {
        b.FlatStyle = FlatStyle.Flat;
        b.FlatAppearance.BorderSize = 1;
        b.Cursor = Cursors.Hand;
        b.Padding = new Padding(10, 3, 10, 3);
        if (primary)
        {
            b.BackColor = Accent;
            b.ForeColor = AccentInk;
            b.FlatAppearance.BorderColor = Accent;
            b.FlatAppearance.MouseOverBackColor = AccentDark;
            b.FlatAppearance.MouseDownBackColor = AccentDarker;
        }
        else
        {
            b.BackColor = Panel;
            b.ForeColor = Ink;
            b.FlatAppearance.BorderColor = Line;
            b.FlatAppearance.MouseOverBackColor = Paper;
            b.FlatAppearance.MouseDownBackColor = Color.FromArgb(230, 234, 236);
        }
    }

    /// <summary>The disabled look StyleButton's primary variant does not
    /// cover on its own: WinForms greys ForeColor automatically but
    /// leaves a flat button's BackColor exactly as set. Also restores
    /// StyleButton's own colours when re-enabled, so a button that was
    /// disabled once does not stay grey after Enabled goes back to
    /// true. `primary` must match the StyleButton call the button was
    /// created with.</summary>
    public static void RefreshButtonEnabled(Button b, bool primary = false)
    {
        if (!b.Enabled)
        {
            b.BackColor = Paper;
            b.ForeColor = Muted;
            b.FlatAppearance.BorderColor = Line;
            return;
        }
        if (primary)
        {
            b.BackColor = Accent;
            b.ForeColor = AccentInk;
            b.FlatAppearance.BorderColor = Accent;
        }
        else
        {
            b.BackColor = Panel;
            b.ForeColor = Ink;
            b.FlatAppearance.BorderColor = Line;
        }
    }
}

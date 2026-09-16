// A GroupBox replacement: WinForms draws a GroupBox's frame and label
// itself, with no hook to recolour either, so a bordered white panel
// with its own header is drawn here instead. Add content to Body, not
// to the CardPanel itself.
using System.Drawing;
using System.Windows.Forms;

namespace PalSign;

public class CardPanel : Panel
{
    private const int HeaderH = 30;

    /// <summary>Everything below the header goes here.</summary>
    public readonly Panel Body;

    public CardPanel(string title)
    {
        BackColor = Theme.Panel;
        Padding = new Padding(1, HeaderH + 1, 1, 1);   // room for the border + header
        // Fill supplies the width (every caller sits in a single column
        // stack), AutoSize the height: the same split GroupBox relied on
        // before this replaced it. Without Dock, AutoSize has no width
        // to measure Body's Dock.Top content against and everything
        // inside collapses to zero.
        Dock = DockStyle.Fill;
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        Margin = new Padding(0, 0, 0, 12);

        Body = new Panel
        {
            Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Padding = new Padding(13, 8, 13, 12), BackColor = Theme.Panel,
        };
        base.Controls.Add(Body);

        var header = new Label
        {
            Text = title, Dock = DockStyle.Top, Height = HeaderH,
            TextAlign = ContentAlignment.MiddleLeft, Padding = new Padding(13, 0, 0, 0),
            Font = new Font(Font, FontStyle.Bold), ForeColor = Theme.Ink, BackColor = Theme.Panel,
        };
        // Sits above Body in the same Dock.Top stack; added last so it
        // claims the top strip first (see MainWindow.cs's own note on
        // Dock.Top ordering).
        base.Controls.Add(header);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        using var pen = new Pen(Theme.Line);
        e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
        e.Graphics.DrawLine(pen, 0, HeaderH, Width - 1, HeaderH);
    }
}

// A small modal window for the read and write progress: modal so nothing
// can be clicked mid-transfer, no close button since breaking one off
// halfway is what the board answers "aborted" to.
using System;
using System.Drawing;
using System.Windows.Forms;

namespace PalSign;

internal sealed class ProgressDialog : Form
{
    private readonly Label _text = new()
    {
        AutoEllipsis = true,
        Location = new Point(12, 10),
        Size = new Size(356, 20),
    };

    private readonly ProgressBar _bar = new()
    {
        Location = new Point(12, 34),
        Size = new Size(356, 22),
        Minimum = 0,
        Maximum = 100,
    };

    /// <summary>marquee = true for work without a known length, such as
    /// the save: the bar then sweeps instead of filling.</summary>
    public ProgressDialog(string title, bool marquee = false)
    {
        if (marquee) _bar.Style = ProgressBarStyle.Marquee;
        Text = title;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        ControlBox = false;
        MinimizeBox = false;
        MaximizeBox = false;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(380, 68);
        Controls.Add(_text);
        Controls.Add(_bar);
    }

    /// <summary>New text and/or a new position. Null keeps the text,
    /// a negative percentage keeps the bar.</summary>
    public void Report(string? text, int percent)
    {
        if (text != null) _text.Text = text;
        if (percent >= 0) _bar.Value = Math.Clamp(percent, 0, 100);
    }

    // A self-walking bar for work the board stays silent during (flash
    // erase can be tens of seconds quiet; see BoardSlots.Erase). Walks a
    // percent per tick up to the cap, then waits for the window to
    // close or for a Report() past the cap to take over. Emptying a slot
    // walks to 95 with no real progress; an upload caps the fake part at
    // 50 and lets sector acknowledgements fill 50..100.
    private readonly Timer _fake = new() { Interval = 100 };

    public void FakePace(int cap = 95, int intervalMs = 100)
    {
        _fake.Interval = intervalMs;
        _fake.Tick += (_, _) => { if (_bar.Value < cap) _bar.Value += 1; };
        _fake.Start();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) _fake.Dispose();
        base.Dispose(disposing);
    }
}

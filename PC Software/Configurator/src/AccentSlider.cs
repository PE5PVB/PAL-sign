// A horizontal slider, drawn rather than the native TrackBar: WinForms
// gives no hook to recolour a TrackBar's own track or thumb, and it is
// the last visibly dated control left after Theme.cs and CardPanel.cs.
// Keeps TrackBar's own property/event names (Minimum, Maximum, Value,
// TickFrequency, SmallChange, LargeChange, ValueChanged, Scroll) so
// every call site swaps in with no change beyond the type name.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace PalSign;

public class AccentSlider : Control
{
    private int _minimum;
    private int _maximum = 10;
    private int _value;
    private bool _dragging;

    public int Minimum
    {
        get => _minimum;
        set { _minimum = value; if (_maximum < value) _maximum = value; ClampValue(); Invalidate(); }
    }

    public int Maximum
    {
        get => _maximum;
        set { _maximum = value; if (_minimum > value) _minimum = value; ClampValue(); Invalidate(); }
    }

    public int TickFrequency { get; set; } = 1;
    public int SmallChange { get; set; } = 1;
    public int LargeChange { get; set; } = 5;

    public int Value
    {
        get => _value;
        set
        {
            int v = Math.Clamp(value, _minimum, _maximum);
            if (v == _value) return;
            _value = v;
            Invalidate();
            ValueChanged?.Invoke(this, EventArgs.Empty);
            Scroll?.Invoke(this, EventArgs.Empty);
        }
    }

    /// <summary>Fired on every change, from user interaction or from
    /// code -- the same as TrackBar. Every call site already guards
    /// programmatic sets with its own _loading flag where that matters.</summary>
    public event EventHandler? ValueChanged;

    /// <summary>Fired together with ValueChanged; TrackBar draws a
    /// distinction between the two that no call site here relied on.</summary>
    public event EventHandler? Scroll;

    private const int ThumbRadius = 8;
    private const int TrackThickness = 4;
    private const int TickHeight = 5;

    public AccentSlider()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                 ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw |
                 ControlStyles.Selectable, true);
        TabStop = true;
        Height = 34;
        Cursor = Cursors.Hand;
    }

    private void ClampValue()
    {
        int v = Math.Clamp(_value, _minimum, _maximum);
        if (v != _value) { _value = v; Invalidate(); }
    }

    // --- geometry ---------------------------------------------------

    private int TrackLeft => ThumbRadius + 2;
    private int TrackRight => Width - ThumbRadius - 2;
    private int TrackY => Height / 2 - 3;   // a little above centre: room for ticks below

    private double FractionOf(int v) =>
        _maximum == _minimum ? 0 : (double)(v - _minimum) / (_maximum - _minimum);

    private int XOf(int v) =>
        TrackRight <= TrackLeft ? TrackLeft : TrackLeft + (int)Math.Round(FractionOf(v) * (TrackRight - TrackLeft));

    private int ValueAtX(int x)
    {
        if (TrackRight <= TrackLeft) return _minimum;
        double f = Math.Clamp((x - TrackLeft) / (double)(TrackRight - TrackLeft), 0.0, 1.0);
        return (int)Math.Round(_minimum + f * (_maximum - _minimum));
    }

    // True for a slider whose range straddles zero (speed/direction
    // style): the filled part reads better from the middle than from
    // the left edge, since zero is the meaningful rest position.
    private bool Bidirectional => _minimum < 0 && _maximum > 0;

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.Clear(Parent?.BackColor ?? Theme.Panel);

        bool on = Enabled;
        Color track = on ? Theme.Line : Theme.Line;
        Color fill = on ? Theme.Accent : Theme.Muted;
        Color thumb = on ? Theme.Accent : Theme.Muted;

        int y = TrackY;
        using (var trackPen = new Pen(track, TrackThickness) { StartCap = LineCap.Round, EndCap = LineCap.Round })
            g.DrawLine(trackPen, TrackLeft, y, TrackRight, y);

        int thumbX = XOf(_value);
        int fillFrom = Bidirectional ? XOf(0) : TrackLeft;
        if (on) using (var fillPen = new Pen(fill, TrackThickness) { StartCap = LineCap.Round, EndCap = LineCap.Round })
            g.DrawLine(fillPen, fillFrom, y, thumbX, y);

        if (TickFrequency > 0)
        {
            using var tickPen = new Pen(Theme.Line, 1);
            for (int t = _minimum; t <= _maximum; t += TickFrequency)
            {
                int tx = XOf(t);
                g.DrawLine(tickPen, tx, y + TrackThickness / 2 + 3, tx, y + TrackThickness / 2 + 3 + TickHeight);
            }
        }

        if (Focused)
        {
            using var halo = new SolidBrush(Color.FromArgb(60, Theme.Accent));
            g.FillEllipse(halo, thumbX - ThumbRadius - 4, y - ThumbRadius - 4 + TrackThickness / 2,
                          (ThumbRadius + 4) * 2, (ThumbRadius + 4) * 2);
        }

        int ty = y + TrackThickness / 2;
        using (var thumbBrush = new SolidBrush(thumb))
            g.FillEllipse(thumbBrush, thumbX - ThumbRadius, ty - ThumbRadius, ThumbRadius * 2, ThumbRadius * 2);
        using (var ring = new Pen(Theme.Panel, 2))
            g.DrawEllipse(ring, thumbX - ThumbRadius + 1, ty - ThumbRadius + 1, ThumbRadius * 2 - 2, ThumbRadius * 2 - 2);
    }

    // --- mouse --------------------------------------------------------

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (!Enabled || e.Button != MouseButtons.Left) return;
        Focus();
        _dragging = true;
        Capture = true;
        Value = ValueAtX(e.X);
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (_dragging) Value = ValueAtX(e.X);
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        _dragging = false;
        Capture = false;
    }

    protected override void OnMouseWheel(MouseEventArgs e)
    {
        base.OnMouseWheel(e);
        if (!Enabled) return;
        Value += Math.Sign(e.Delta) * SmallChange;
    }

    // --- keyboard -------------------------------------------------------

    protected override bool IsInputKey(Keys keyData) => keyData switch
    {
        Keys.Left or Keys.Right or Keys.Up or Keys.Down or Keys.Home or Keys.End
            or Keys.PageUp or Keys.PageDown => true,
        _ => base.IsInputKey(keyData),
    };

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (!Enabled) return;
        switch (e.KeyCode)
        {
            case Keys.Left: case Keys.Down: Value -= SmallChange; break;
            case Keys.Right: case Keys.Up: Value += SmallChange; break;
            case Keys.PageDown: Value -= LargeChange; break;
            case Keys.PageUp: Value += LargeChange; break;
            case Keys.Home: Value = Minimum; break;
            case Keys.End: Value = Maximum; break;
            default: return;
        }
        e.Handled = true;
    }

    protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); Invalidate(); }
    protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); Invalidate(); }
    protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Invalidate(); }
    protected override void OnResize(EventArgs e) { base.OnResize(e); Invalidate(); }
}

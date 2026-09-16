// Tab page: the settings that are the same whatever card is showing.
// Per-card settings (ticker, aspect, inserts, identification) live on
// CardsTab instead.
//
// Text boxes send on Leave/Enter, not per keystroke; switches and lists
// send at once. Nothing reaches flash until "Save to flash": the
// firmware only erases on command, which blacks the picture briefly.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Threading;
using System.Windows.Forms;

namespace PalSign;

/// <summary>A NumericUpDown that always shows the same digit count, so
/// the contest number reads 0001 and not 1, matching what goes out on
/// air.</summary>
public class FixedDigits : NumericUpDown
{
    public int Digits = 4;

    protected override void UpdateEditText() =>
        Text = ((int)Value).ToString(new string('0', Digits), CultureInfo.InvariantCulture);
}

/// <summary>A scrolling panel whose vertical scrollbar is always shown,
/// even greyed out, so it is visible before it is needed. Style bit
/// WS_VSCROLL, 0x00200000 in winuser.h.</summary>
public class ScrollHost : Panel
{
    protected override CreateParams CreateParams
    {
        get
        {
            var cp = base.CreateParams;
            cp.Style |= 0x00200000;   // WS_VSCROLL
            return cp;
        }
    }
}

public class SystemTab : TabPage
{
    private readonly Func<string?> _port;

    private readonly TextBox _textId = new() { Dock = DockStyle.Fill, MaxLength = 31 };
    private readonly TextBox _textSub = new() { Dock = DockStyle.Fill, MaxLength = 31 };
    private readonly TextBox _tickerText = new() { Dock = DockStyle.Fill, MaxLength = 96 };
    // Ticker colour/speed/height/font and the contest texts are per
    // card, on CardsTab; only the ticker message itself is shared.
    private readonly TextBox _insertLeft = new() { Dock = DockStyle.Fill, MaxLength = 31 };
    private readonly TextBox _insertRight = new() { Dock = DockStyle.Fill, MaxLength = 31 };

    private readonly CheckBox _teletext = new() { Text = UiText.TeletextSwitch, AutoSize = true };
    private readonly Button _teletextEdit = new()
        { Text = UiText.TeletextEditButton, AutoSize = true, Margin = new Padding(20, 0, 0, 4) };
    private readonly CheckBox _its = new() { Text = UiText.ItsSwitch, AutoSize = true };
    private readonly CheckBox _vbi = new() { Text = UiText.VbiSwitch, AutoSize = true };
    private readonly CheckBox _slideFade = new() { Text = UiText.SlideFadeSwitch, AutoSize = true };
    private readonly CheckBox _dstAuto = new() { Text = UiText.DstAutoSwitch, AutoSize = true };
    // Re-reads the board, so it needs one to read; SetEnabled(false)
    // at construction time matches Forget() before the first Connect.
    private readonly Button _fetchAll = new() { Text = UiText.FetchAllAgain, AutoSize = true, Enabled = false };
    // Switches that belong to one card each stand on CardsTab instead.

    private readonly ComboBox _luma = new()
        { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
    private readonly ComboBox _chroma = new()
        { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
    // No "starts on" box: the board writes the current card to flash on
    // every card change, so a stored choice would just be overwritten.

    private readonly Label _state = new() { AutoSize = true, Margin = new Padding(8, 6, 0, 0) };

    // The board's own little display panel, not the video signal.
    private readonly AccentSlider _bright = new()
    {
        Minimum = 0, Maximum = 100, TickFrequency = 10, SmallChange = 5, LargeChange = 10,
        Value = 100, Dock = DockStyle.Fill, Height = 34,
    };
    private readonly Label _brightValue = new() { AutoSize = true, Margin = new Padding(6, 9, 0, 0) };

    // The encoder controls, built from what the board reports (range and
    // neutral value per control), so a control added later needs no code
    // here and this side keeps no copy of the firmware's ranges.
    private readonly TableLayoutPanel _controls = new()
        { Dock = DockStyle.Fill, ColumnCount = 4, AutoSize = true, Padding = new Padding(10) };

    // Queued and sent one a tick over a port held open, same as CardsTab.
    private readonly System.Windows.Forms.Timer _tick = new() { Interval = 40 };
    private readonly Dictionary<string, string> _pending = new();
    private System.IO.Ports.SerialPort? _live;
    private int _quiet;

    /// <summary>Something changed that may alter what a card looks
    /// like; see CardsTab.NoteSharedChange().</summary>
    public Action? Changed;

    // Fetch all again button; the work lives in CardsTab.
    public Action? FetchAll;

    // Update firmware button; the work lives in MainWindow, which owns
    // the shared progress dialog and the port the same way Save does.
    public Action? UpdateFirmware;

    // Set by MainWindow to the shared status label on the tab strip,
    // which unlike _state never scrolls out of view; see Say().
    public Action<string>? Status;

    private bool _refreshDue;

    private BoardSettings? _cfg;
    private bool _loading;

    public SystemTab(Func<string?> port)
    {
        _port = port;
        Text = UiText.TabSystem;
        BackColor = Theme.Paper;
        Padding = new Padding(12);

        // Lettered on the television, so limited to what the board's
        // faces can draw; see BoardText.
        foreach (var b in new[] { _textId, _textSub, _tickerText, _insertLeft, _insertRight })
            BoardText.Guard(b);
        Text_(_textId, "textid");
        Text_(_textSub, "textsub");
        Text_(_tickerText, "tickertext");
        Text_(_insertLeft, "insertleft");
        Text_(_insertRight, "insertright");

        Switch(_teletext, "teletext");
        Theme.StyleButton(_teletextEdit);
        _teletextEdit.Click += (_, __) => EditTeletext();
        Switch(_its, "its");
        Switch(_vbi, "vbi");
        Switch(_slideFade, "slidefade");
        Switch(_dstAuto, "dstauto");

        _luma.SelectedIndexChanged += (_, __) => Push("luma", _luma.SelectedIndex.ToString());
        _chroma.SelectedIndexChanged += (_, __) => Push("chroma", _chroma.SelectedIndex.ToString());
        _bright.ValueChanged += (_, __) =>
        {
            _brightValue.Text = _bright.Value.ToString(CultureInfo.InvariantCulture) + " %";
            Queue("displaybright", _bright.Value.ToString(CultureInfo.InvariantCulture));
        };


        _tick.Tick += (_, __) => Drain();
        _tick.Start();

        Controls.Add(BuildLayout());
        SetEnabled(false);
    }

    // Sends only on a real change; last-sent value kept in Tag.
    private void Text_(TextBox box, string name)
    {
        void Send()
        {
            if (_loading) return;
            if (box.Tag as string == box.Text) return;
            Push(name, box.Text);
            box.Tag = box.Text;   // after queueing, so a failed send is retried
        }
        box.Leave += (_, __) => Send();
        box.KeyDown += (_, e) =>
        {
            if (e.KeyCode != Keys.Enter) return;
            e.SuppressKeyPress = true;      // no ding from WinForms
            Send();
        };
    }

    private void Switch(CheckBox box, string name) =>
        box.CheckedChanged += (_, __) => Push(name, box.Checked ? "1" : "0");

    private Control BuildLayout()
    {
        // A ScrollHost wraps the table rather than the table scrolling
        // itself: a TableLayoutPanel docked to Fill never reports the
        // height it wants, so AutoScroll on it never triggers.
        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Top, ColumnCount = 1,
            AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
        };
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        root.Controls.Add(Group(UiText.GroupTexts, new (string, Control)[]
        {
            (UiText.LabelFirstLine, _textId),
            (UiText.LabelSecondLine, _textSub),
            (UiText.LabelTickerText, _tickerText),
            // Take the place of the date and time when a card's insert
            // boxes are set to fixed texts; named after what they replace.
            (UiText.LabelTextDate, _insertLeft),
            (UiText.LabelTextTime, _insertRight),
        }));

        root.Controls.Add(Group(UiText.GroupSignal, new (string, Control)[]
        {
            (UiText.LabelLumaFilter, _luma),
            (UiText.LabelChromaFilter, _chroma),
        }));

        var panel = new TableLayoutPanel
            { Dock = DockStyle.Top, ColumnCount = 2, AutoSize = true };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 56));
        panel.Controls.Add(_bright);
        panel.Controls.Add(_brightValue);
        panel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        var panelBox = new CardPanel(UiText.GroupBrightness);
        panelBox.Body.Controls.Add(panel);
        root.Controls.Add(panelBox);
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var picture = new CardPanel(UiText.GroupControls);
        _controls.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 110));
        _controls.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        _controls.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 56));
        _controls.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _controls.Padding = new Padding(0);
        _controls.Dock = DockStyle.Top;
        picture.Body.Controls.Add(_controls);
        root.Controls.Add(picture);
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var flags = new FlowLayoutPanel
        {
            Dock = DockStyle.Top, FlowDirection = FlowDirection.TopDown,
            AutoSize = true, WrapContents = false,
        };
        flags.Controls.Add(_vbi);
        flags.Controls.Add(_teletext);
        flags.Controls.Add(_teletextEdit);
        flags.Controls.Add(_its);
        flags.Controls.Add(_slideFade);
        flags.Controls.Add(_dstAuto);
        var box = new CardPanel(UiText.GroupSwitches);
        box.Body.Controls.Add(flags);
        root.Controls.Add(box);

        var buttons = new FlowLayoutPanel
            { Dock = DockStyle.Fill, AutoSize = true, Margin = new Padding(0, 4, 0, 0) };
        buttons.Controls.Add(_state);
        root.Controls.Add(buttons);
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        // Rare, heavy actions, kept off the everyday CardsTab buttons.
        var fetchRow = new FlowLayoutPanel
            { Dock = DockStyle.Fill, AutoSize = true, WrapContents = false,
              Margin = new Padding(0, 12, 0, 0) };
        Theme.StyleButton(_fetchAll);
        _fetchAll.Click += (_, __) => FetchAll?.Invoke();
        var wireLog = new Button { Text = UiText.LogButton, AutoSize = true,
                                   Margin = new Padding(8, 0, 0, 0) };
        Theme.StyleButton(wireLog);
        wireLog.Click += (_, __) => Trace.Show();
        var updateFw = new Button { Text = UiText.FirmwareUpdateButton, AutoSize = true,
                                    Margin = new Padding(8, 0, 0, 0) };
        Theme.StyleButton(updateFw);
        updateFw.Click += (_, __) => UpdateFirmware?.Invoke();
        fetchRow.Controls.Add(_fetchAll);
        fetchRow.Controls.Add(wireLog);
        fetchRow.Controls.Add(updateFw);
        root.Controls.Add(fetchRow);
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var scroller = new ScrollHost { Dock = DockStyle.Fill, AutoScroll = true, BackColor = Theme.Paper };
        scroller.Controls.Add(root);
        return scroller;
    }

    private static Control Group(string title, (string, Control)[] rows)
    {
        var t = new TableLayoutPanel
        {
            Dock = DockStyle.Top, ColumnCount = 2, AutoSize = true,
        };
        t.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 110));
        t.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        foreach (var (label, c) in rows)
        {
            t.Controls.Add(new Label
                { Text = label, AutoSize = true, ForeColor = Theme.Ink, Margin = new Padding(0, 6, 8, 0) });
            t.Controls.Add(c);
            t.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        }
        var box = new CardPanel(title);
        box.Body.Controls.Add(t);
        return box;
    }

    // Built from what the board describes, not written out, so a control
    // added later needs no change here.
    private void BuildControls(BoardSettings cfg)
    {
        _controls.SuspendLayout();
        // Disposed, not just cleared: Clear() alone leaves window handles
        // alive, and this runs once per Connect.
        foreach (Control old in _controls.Controls) old.Dispose();
        _controls.Controls.Clear();
        _controls.RowStyles.Clear();

        foreach (var c in cfg.Controls)
        {
            var bar = new AccentSlider
            {
                Minimum = c.Min, Maximum = c.Max,
                TickFrequency = Math.Max(1, (c.Max - c.Min) / 20),   // about 20 marks
                SmallChange = c.Step, LargeChange = Math.Max(c.Step, (c.Max - c.Min) / 10),
                Value = Math.Clamp(c.Value, c.Min, c.Max),
                Dock = DockStyle.Fill, Height = 34,
            };
            var value = new Label
                { AutoSize = true, Margin = new Padding(6, 9, 0, 0),
                  Text = c.Value.ToString(CultureInfo.InvariantCulture) };
            // Resets to neutral in one press (e.g. hue: 0 of -128..127).
            var reset = new Button
                { Text = UiText.NeutralButton, AutoSize = true, Margin = new Padding(8, 5, 0, 0) };

            var info = c;
            bar.ValueChanged += (_, __) =>
            {
                value.Text = bar.Value.ToString(CultureInfo.InvariantCulture);
                info.Value = bar.Value;
                Queue($"control.{info.Index}", bar.Value.ToString(CultureInfo.InvariantCulture));
            };
            reset.Click += (_, __) => bar.Value = Math.Clamp(info.Neutral, info.Min, info.Max);

            _controls.Controls.Add(new Label
                { Text = Capital(c.Name), AutoSize = true, Margin = new Padding(0, 9, 8, 0) });
            _controls.Controls.Add(bar);
            _controls.Controls.Add(value);
            _controls.Controls.Add(reset);
            _controls.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        }
        _controls.ResumeLayout();
    }

    // The board sends control names lower case, for a terminal line.
    private static string Capital(string s) =>
        s.Length == 0 ? s : char.ToUpperInvariant(s[0]) + s[1..];

    // Keyed by name, so a value set again before it went over replaces
    // the one waiting rather than queueing behind it.
    private void Queue(string name, string value)
    {
        if (_loading) return;
        _pending[name] = value;
    }

    // One a tick: this runs on the drawing thread.
    private void Drain()
    {
        if (_pending.Count == 0)
        {
            // Once the queue is dry, not per change: avoids a picture
            // refresh per slider step.
            if (_refreshDue) { _refreshDue = false; Changed?.Invoke(); }
            // Let the port go after a second of quiet, for the other tab.
            if (_live != null && ++_quiet > 25) DropLive();
            return;
        }
        _quiet = 0;
        string? p = _port();
        if (p == null) { _pending.Clear(); return; }

        string name = "";
        foreach (var k in _pending.Keys) { name = k; break; }
        string value = _pending[name];
        _pending.Remove(name);

        try
        {
            _live ??= BoardConfig.OpenLive(p);
            BoardConfig.SetLive(_live, name, value);
            Session.Dirty = true;
            _refreshDue = true;   // silent on success; only failures are reported
        }
        catch (Exception ex)
        {
            // Only the failed item is dropped, already removed above;
            // clearing the whole queue would lose every other pending
            // change. Same rule as CardsTab.Drain().
            DropLive();
            Say(ex.Message);
        }
    }

    /// <summary>Lets go of the port held open for live changes.</summary>
    public void ReleasePort() => DropLive();

    private void DropLive()
    {
        if (_live == null) return;
        BoardConfig.CloseLive(_live);
        _live = null;
        _quiet = 0;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) { _tick.Stop(); DropLive(); }
        base.Dispose(disposing);
    }

    // Same queue as the sliders: opening a second port while one is held
    // open for a dragged slider fails outright. Mirrored into _cfg too,
    // since CardsTab's preview reads _cfg.TickerText directly.
    private void Push(string name, string value)
    {
        if (_cfg != null)
        {
            switch (name)
            {
                case "textid": _cfg.TextId = value; break;
                case "textsub": _cfg.TextSub = value; break;
                case "tickertext": _cfg.TickerText = value; break;
                case "insertleft": _cfg.InsertLeft = value; break;
                case "insertright": _cfg.InsertRight = value; break;
            }
        }
        Queue(name, value);
    }

    /// <summary>Lets go of the board: everything read off it is
    /// forgotten and nothing here can be set until the next Connect.
    /// </summary>
    public void Forget()
    {
        DropLive();
        _pending.Clear();
        _refreshDue = false;
        _cfg = null;
        SetEnabled(false);
        Say("");
    }

    /// <summary>Takes what Connect read off the board.</summary>
    public void Adopt(BoardSettings cfg)
    {
        _cfg = cfg;

        _loading = true;
        try
        {
            _textId.Text = cfg.TextId;
            _textSub.Text = cfg.TextSub;
            _tickerText.Text = cfg.TickerText;
            _insertLeft.Text = cfg.InsertLeft;
            _insertRight.Text = cfg.InsertRight;
            // What came off the board counts as sent, so leaving a box
            // untouched sends nothing.
            foreach (var b in new[] { _textId, _textSub, _tickerText,
                                      _insertLeft, _insertRight })
                b.Tag = b.Text;
            _bright.Value = Math.Clamp(cfg.DisplayBright, _bright.Minimum, _bright.Maximum);
            _brightValue.Text = _bright.Value.ToString(CultureInfo.InvariantCulture) + " %";
            BuildControls(cfg);

            _teletext.Checked = cfg.Teletext;
            _its.Checked = cfg.Its;
            _vbi.Checked = cfg.Vbi;
            _slideFade.Checked = cfg.SlideFadeOn;
            _dstAuto.Checked = cfg.DstAutoOn;

            Fill(_luma, cfg.LumaNames, cfg.Luma);
            Fill(_chroma, cfg.ChromaNames, cfg.Chroma);
        }
        finally { _loading = false; }

        SetEnabled(true);
        Say("");   // CardsTab already reports what Connect did
    }

    // Opens the teletext editor. Releases the live port first: the
    // dialog opens its own, same reason MainWindow.SaveToFlash() does.
    private void EditTeletext()
    {
        if (_cfg == null) return;
        ReleasePort();
        using var d = new TeletextDialog(_port, _cfg.TeletextRows);
        if (d.ShowDialog(FindForm()) == DialogResult.OK) _cfg.TeletextRows = d.Result;
    }

    private static void Fill(ComboBox box, System.Collections.Generic.List<string> names, int chosen)
    {
        box.Items.Clear();
        foreach (var n in names) box.Items.Add(n);
        if (box.Items.Count > 0) box.SelectedIndex = Math.Clamp(chosen, 0, box.Items.Count - 1);
    }

    private void SetEnabled(bool on)
    {
        foreach (Control c in new Control[]
                 { _textId, _textSub, _tickerText, _insertLeft, _insertRight,
                   _teletext, _teletextEdit, _its, _vbi, _slideFade, _dstAuto, _luma, _chroma, _bright,
                   _fetchAll })
            c.Enabled = on;
        _controls.Enabled = on;
    }

    private void Say(string s)
    {
        _state.Text = s;
        // Only non-empty goes to the shared label, so clearing this
        // page's own marker cannot blank Connect's message there too.
        if (s.Length > 0) Status?.Invoke(s);
    }
}

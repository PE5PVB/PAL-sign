// Tab page: the slideshow, built by dragging cards into an order.
//
// Left list is every card from Connect, right list is the sequence.
// Add by drag or double click, reorder by dragging within the right
// list, remove by double click, Delete or the button. A card dropped in
// takes the current value of the seconds box.
//
// The wire form is one text line, "2s:1/3s:12/...", and the board does
// the validation. Write serialises and reads the result back, so what
// is shown always matches what the board holds. Limits mirror the
// firmware: 32 steps (SEQ_MAX_STEPS), 65.5 s a step (uint16 thousandths).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace PalSign;

public class SlideshowTab : TabPage
{
    private sealed class Step
    {
        public int Card;          // 1 based, the number the board lists
        public double Seconds;
    }

    private const int MaxSteps = 32;   // SEQ_MAX_STEPS in Firmware/src/settings.h

    // The drag payloads. Named formats, so text dragged in from another
    // program can never be mistaken for one of ours.
    private const string FmtCard = "palsign-card";
    private const string FmtStep = "palsign-step";

    private readonly Func<string?> _port;
    private BoardSettings? _cfg;
    private readonly List<Step> _steps = new();
    private bool _loading;

    // True when _steps holds something the board does not, separately
    // from Session.Dirty (unsaved-to-flash). Set by every real edit,
    // cleared by Adopt() and a successful Parse().
    private bool _dirty;
    public bool Dirty => _dirty;

    // Set by MainWindow to its own SaveToFlash(), so Write to board
    // finishes the job in one click.
    public Action? Save { get; set; }

    // Set by MainWindow to CardsTab.SetSlideshowRunning(), so the card
    // list and the preview there can follow a slideshow the board is
    // stepping through on its own, which nothing else ever re-asks for.
    public Action<bool>? RunningChanged { get; set; }

    private readonly ListBox _cards = new()
    {
        Dock = DockStyle.Fill,
        IntegralHeight = false,
        Font = new Font(FontFamily.GenericMonospace, 10f),
    };
    private readonly ListBox _seq = new()
    {
        Dock = DockStyle.Fill,
        IntegralHeight = false,
        AllowDrop = true,
        Font = new Font(FontFamily.GenericMonospace, 10f),
    };
    private readonly NumericUpDown _secs = new()
    {
        DecimalPlaces = 1,
        Increment = 0.5m,
        Minimum = 0.1m,
        Maximum = 65.5m,   // the step time is a uint16 in thousandths
        Value = 2.0m,
        Width = 70,
        Margin = new Padding(8, 2, 0, 0),
    };
    private readonly Button _remove = new()
        { Text = UiText.RemoveStep, AutoSize = true, Margin = new Padding(16, 0, 0, 0) };
    // Start or stop, one button that says which of the two it will do.
    // The board keeps the running state in flash beside the card, so a
    // show that is on survives a power cut until it is stopped here or
    // with key s.
    private readonly Button _run = new()
        { Text = UiText.StartSlideshow, AutoSize = true, Margin = new Padding(0, 0, 16, 0) };
    private bool _running;

    private readonly Button _read = new() { Text = UiText.ReadFromBoard, AutoSize = true, Margin = new Padding(0, 0, 8, 0) };
    private readonly Button _write = new() { Text = UiText.WriteToBoard, AutoSize = true, Margin = new Padding(0, 0, 8, 0) };
    private readonly Button _clear = new() { Text = UiText.ClearButton, AutoSize = true, Margin = new Padding(0, 0, 8, 0) };
    private readonly TextBox _log = new()
    {
        Multiline = true,
        ScrollBars = ScrollBars.Vertical,
        ReadOnly = true,
        Dock = DockStyle.Fill,
        Margin = new Padding(0, 8, 0, 0),
        BackColor = SystemColors.Window,
        MinimumSize = new Size(0, 90),
    };

    // Where a drag might start, and on which row. The drag itself only
    // begins once the mouse has moved past SystemInformation.DragSize,
    // so a plain click keeps selecting.
    private Point _down;
    private int _downCard = -1;
    private int _downStep = -1;

    public SlideshowTab(Func<string?> port)
    {
        _port = port;
        Text = UiText.TabSlideshow;
        BackColor = Theme.Paper;
        Padding = new Padding(12);
        Theme.StyleButton(_run, primary: true);
        Theme.StyleButton(_read);
        Theme.StyleButton(_write);
        Theme.StyleButton(_clear);
        Theme.StyleButton(_remove);

        var table = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5 };
        table.RowStyles.Add(new RowStyle(SizeType.AutoSize));        // the explanation
        table.RowStyles.Add(new RowStyle(SizeType.Percent, 100));    // the two lists
        table.RowStyles.Add(new RowStyle(SizeType.AutoSize));        // the seconds
        table.RowStyles.Add(new RowStyle(SizeType.AutoSize));        // the buttons
        table.RowStyles.Add(new RowStyle(SizeType.AutoSize));        // the log

        table.Controls.Add(new Label
        {
            Text = UiText.SlideshowExplain,
            AutoSize = true,
            ForeColor = Theme.Muted,
            Margin = new Padding(0, 0, 0, 8),
        }, 0, 0);

        var lists = new TableLayoutPanel
            { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 2, Margin = new Padding(0) };
        lists.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        lists.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        lists.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        lists.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        lists.Controls.Add(new Label
            { Text = UiText.SlideshowCardsLabel, AutoSize = true, Font = new Font(Font, FontStyle.Bold), ForeColor = Theme.Ink }, 0, 0);
        lists.Controls.Add(new Label
            { Text = UiText.SlideshowStepsLabel, AutoSize = true, Font = new Font(Font, FontStyle.Bold), ForeColor = Theme.Ink }, 1, 0);
        _cards.Margin = new Padding(0, 2, 6, 0);
        _seq.Margin = new Padding(6, 2, 0, 0);
        lists.Controls.Add(_cards, 0, 1);
        lists.Controls.Add(_seq, 1, 1);
        table.Controls.Add(lists, 0, 1);

        var secsRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 8, 0, 0) };
        secsRow.Controls.Add(new Label
            { Text = UiText.SecondsLabel, AutoSize = true, Margin = new Padding(0, 6, 0, 0) });
        secsRow.Controls.Add(_secs);
        secsRow.Controls.Add(_remove);
        table.Controls.Add(secsRow, 0, 2);

        var bar = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 8, 0, 8) };
        bar.Controls.Add(_run);
        bar.Controls.Add(_read);
        bar.Controls.Add(_write);
        bar.Controls.Add(_clear);
        table.Controls.Add(bar, 0, 3);

        table.Controls.Add(_log, 0, 4);
        Controls.Add(table);

        WireDragAndDrop();
        _run.Click += (_, _) => ToggleRun();
        _read.Click += (_, _) => ReadFromBoard();
        _write.Click += (_, _) => WriteToBoard();
        _clear.Click += (_, _) =>
        {
            if (_steps.Count == 0) return;   // nothing to confirm
            if (MessageBox.Show(FindForm(), UiText.SlideshowClearAsk, UiText.SlideshowClearTitle,
                                MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK)
                return;
            _steps.Clear();
            _dirty = true;
            RefreshSteps(-1);
        };
        _remove.Click += (_, _) => RemoveSelected();
        _seq.KeyDown += (_, e) => { if (e.KeyCode == Keys.Delete) { RemoveSelected(); e.Handled = true; } };
        _seq.SelectedIndexChanged += (_, _) => ShowSeconds();
        _secs.ValueChanged += (_, _) => TakeSeconds();
        _seq.DoubleClick += (_, _) => RemoveSelected();

        SetLive(false);
    }

    /// <summary>Takes what Connect read off the board: the names for the
    /// left list. The sequence itself is not in that dump; the Read
    /// button asks for it, over a port that is free by then.</summary>
    public void Adopt(BoardSettings cfg)
    {
        _cfg = cfg;
        _cards.BeginUpdate();
        _cards.Items.Clear();
        foreach (var c in cfg.Cards) _cards.Items.Add($"{c.Number,2}  {c.Name}");
        _cards.EndUpdate();
        _running = cfg.SlideshowOn;
        _run.Text = _running ? UiText.StopSlideshow : UiText.StartSlideshow;
        RefreshSteps(_seq.SelectedIndex);   // the names in the steps may have changed
        _dirty = false;   // a fresh Connect, nothing local yet to lose
        SetLive(true);
        RunningChanged?.Invoke(_running);
    }

    /// <summary>Lets go of the board, the counterpart of Adopt: without
    /// the card names there is nothing to drag.</summary>
    public void Forget()
    {
        _cfg = null;
        _cards.Items.Clear();
        _steps.Clear();
        RefreshSteps(-1);
        SetLive(false);
        if (_running) { _running = false; RunningChanged?.Invoke(false); }
    }

    private void SetLive(bool on)
    {
        foreach (Control c in new Control[] { _cards, _seq, _secs, _remove, _run, _read, _write, _clear })
            c.Enabled = on;
        // _run alone: the only primary (accent-filled) button in this
        // group. Without this it stayed solid blue while disconnected,
        // same fix as CardsTab's _upload; see Theme.RefreshButtonEnabled.
        Theme.RefreshButtonEnabled(_run, primary: true);
    }

    // --- the list of steps ----------------------------------------------

    private string NameOf(int card)
    {
        if (_cfg != null)
            foreach (var c in _cfg.Cards)
                if (c.Number == card) return c.Name;
        return "";
    }

    private static string SecondsText(double s) => s.ToString("0.0", CultureInfo.InvariantCulture);

    private void RefreshSteps(int select)
    {
        _loading = true;
        _seq.BeginUpdate();
        _seq.Items.Clear();
        for (int i = 0; i < _steps.Count; i++)
        {
            var s = _steps[i];
            _seq.Items.Add($"{i + 1,2}. {SecondsText(s.Seconds),5} s  {s.Card,2}  {NameOf(s.Card)}");
        }
        _seq.EndUpdate();
        if (select >= _steps.Count) select = _steps.Count - 1;
        _seq.SelectedIndex = select;
        _loading = false;
        ShowSeconds();
    }

    private void ShowSeconds()
    {
        if (_loading) return;
        int i = _seq.SelectedIndex;
        if (i < 0 || i >= _steps.Count) return;
        _loading = true;
        _secs.Value = Math.Clamp((decimal)_steps[i].Seconds, _secs.Minimum, _secs.Maximum);
        _loading = false;
    }

    private void TakeSeconds()
    {
        if (_loading) return;
        int i = _seq.SelectedIndex;
        if (i < 0 || i >= _steps.Count) return;
        _steps[i].Seconds = (double)_secs.Value;
        _dirty = true;
        RefreshSteps(i);
    }

    private void AddStep(int card, int at)
    {
        // The firmware refuses more than SEQ_MAX_STEPS in one line, so
        // this page stops at the same wall instead of building a line
        // the board will bounce.
        if (_steps.Count >= MaxSteps)
        {
            Report(string.Format(UiText.StepsFullFmt, MaxSteps));
            return;
        }
        if (at < 0 || at > _steps.Count) at = _steps.Count;
        _steps.Insert(at, new Step { Card = card, Seconds = (double)_secs.Value });
        _dirty = true;
        Report($"added step {at + 1}: {NameOf(card)}");
        RefreshSteps(at);
    }

    private void RemoveSelected()
    {
        // Reported in the log rather than confirmed: removing a step is
        // easily undone (drag the card back in) and reachable three
        // ways, so a dialog on every double click would be worse than
        // the mistake it guards against.
        int i = _seq.SelectedIndex;
        if (i < 0 || i >= _steps.Count) return;
        string removed = NameOf(_steps[i].Card);
        _steps.RemoveAt(i);
        _dirty = true;
        Report($"removed step {i + 1}: {removed}");
        RefreshSteps(Math.Min(i, _steps.Count - 1));
    }

    private int CardAt(int index) =>
        _cfg != null && index >= 0 && index < _cfg.Cards.Count ? _cfg.Cards[index].Number : -1;

    // --- drag and drop --------------------------------------------------

    private void WireDragAndDrop()
    {
        _cards.MouseDown += (_, e) => { _down = e.Location; _downCard = _cards.IndexFromPoint(e.Location); };
        _cards.MouseUp += (_, _) => _downCard = -1;
        _cards.MouseMove += (_, e) =>
        {
            if (e.Button != MouseButtons.Left || _downCard < 0 || !Dragged(e.Location)) return;
            int card = CardAt(_downCard);
            _downCard = -1;
            if (card > 0) _cards.DoDragDrop(new DataObject(FmtCard, card), DragDropEffects.Copy);
        };
        _cards.DoubleClick += (_, _) =>
        {
            int card = CardAt(_cards.SelectedIndex);
            if (card > 0) AddStep(card, _steps.Count);
        };

        _seq.MouseDown += (_, e) => { _down = e.Location; _downStep = _seq.IndexFromPoint(e.Location); };
        _seq.MouseUp += (_, _) => _downStep = -1;
        _seq.MouseMove += (_, e) =>
        {
            if (e.Button != MouseButtons.Left || _downStep < 0 || !Dragged(e.Location)) return;
            int from = _downStep;
            _downStep = -1;
            _seq.DoDragDrop(new DataObject(FmtStep, from), DragDropEffects.Move);
        };

        _seq.DragOver += (_, e) =>
        {
            e.Effect = e.Data == null ? DragDropEffects.None
                     : e.Data.GetDataPresent(FmtCard) ? DragDropEffects.Copy
                     : e.Data.GetDataPresent(FmtStep) ? DragDropEffects.Move
                     : DragDropEffects.None;
        };
        _seq.DragDrop += (_, e) =>
        {
            if (e.Data == null) return;
            // Dropped past the last row means "at the end".
            int at = _seq.IndexFromPoint(_seq.PointToClient(new Point(e.X, e.Y)));
            if (at < 0) at = _steps.Count;
            if (e.Data.GetDataPresent(FmtCard))
            {
                AddStep((int)e.Data.GetData(FmtCard)!, at);
            }
            else if (e.Data.GetDataPresent(FmtStep))
            {
                int from = (int)e.Data.GetData(FmtStep)!;
                if (from < 0 || from >= _steps.Count) return;
                var s = _steps[from];
                _steps.RemoveAt(from);
                if (at > from) at--;
                if (at > _steps.Count) at = _steps.Count;
                _steps.Insert(at, s);
                _dirty = true;
                RefreshSteps(at);
            }
        };
    }

    private bool Dragged(Point now)
    {
        Size min = SystemInformation.DragSize;
        return Math.Abs(now.X - _down.X) >= min.Width || Math.Abs(now.Y - _down.Y) >= min.Height;
    }

    // --- the wire -------------------------------------------------------

    private string Serialise()
    {
        var b = new StringBuilder();
        foreach (var s in _steps)
        {
            if (b.Length > 0) b.Append('/');
            b.Append(s.Seconds.ToString("0.###", CultureInfo.InvariantCulture));
            b.Append("s:");
            b.Append(s.Card);
        }
        return b.ToString();
    }

    /// <summary>The board's own form back into steps: "2s:1/1.5s:12".
    /// The board writes out what it accepted, so this only has to read
    /// what the board writes; a part that does not fit is reported and
    /// the parse gives up rather than guessing.</summary>
    private bool Parse(string text)
    {
        _steps.Clear();
        text = text.Trim();
        if (text.Length == 0) return true;
        foreach (string part in text.Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            string p = part.Trim();
            int colon = p.IndexOf(':');
            string t = colon >= 0 ? p[..colon].Trim() : "";
            if (t.EndsWith("s", StringComparison.OrdinalIgnoreCase)) t = t[..^1].Trim();
            if (colon < 0
                || !double.TryParse(t, NumberStyles.Float, CultureInfo.InvariantCulture, out double secs)
                || !int.TryParse(p[(colon + 1)..].Trim(), out int card))
            {
                Report(string.Format(UiText.StepNotUnderstoodFmt, p));
                _steps.Clear();
                return false;
            }
            _steps.Add(new Step { Card = card, Seconds = secs });
        }
        return true;
    }

    private void Report(string text) => _log.AppendText(text + Environment.NewLine);

    private string? Port()
    {
        string? p = _port();
        if (string.IsNullOrEmpty(p))
        {
            MessageBox.Show(this, UiText.ChoosePortFirst, UiText.AppTitle);
            return null;
        }
        return p;
    }

    private void Busy(bool on) => _read.Enabled = _write.Enabled = _run.Enabled = !on;

    private void ToggleRun()
    {
        string? port = Port();
        if (port == null) return;
        bool want = !_running;
        Report(want ? "--- starting the slideshow ---" : "--- stopping the slideshow ---");
        Background(() =>
        {
            BoardConfig.Set(port, "slideshow", want ? "1" : "0", CancellationToken.None);
            BeginInvoke(() =>
            {
                _running = want;
                _run.Text = _running ? UiText.StopSlideshow : UiText.StartSlideshow;
                Report(_running ? "running; it survives a power cut until it is stopped"
                                : "stopped");
                RunningChanged?.Invoke(_running);
            });
        });
    }

    /// <summary>On its own thread, so the window does not freeze while the
    /// board is written to. Everything that touches a control goes back
    /// through BeginInvoke.</summary>
    private void Background(Action work)
    {
        Busy(true);
        var thread = new Thread(() =>
        {
            try
            {
                work();
                BeginInvoke(() => Busy(false));
            }
            catch (Exception e)
            {
                BeginInvoke(() =>
                {
                    Report("FAILED: " + e.Message);
                    Busy(false);
                    MessageBox.Show(FindForm(), e.Message, UiText.AppTitle,
                                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                });
            }
        })
        { IsBackground = true };
        thread.Start();
    }

    private void ReadFromBoard()
    {
        if (_dirty && MessageBox.Show(FindForm(), UiText.SlideshowUnwrittenAsk,
                                      UiText.SlideshowUnwrittenTitle, MessageBoxButtons.YesNo,
                                      MessageBoxIcon.Warning) != DialogResult.Yes)
            return;
        string? port = Port();
        if (port == null) return;
        Report("--- reading the sequence ---");
        Background(() =>
        {
            string text = BoardSlots.ReadSequence(port, CancellationToken.None);
            BeginInvoke(() =>
            {
                Report(text.Length > 0 ? "on the board: " + text : "the board has no sequence");
                Parse(text);
                _dirty = false;   // local now matches the board again
                RefreshSteps(_steps.Count > 0 ? 0 : -1);
            });
        });
    }

    private void WriteToBoard()
    {
        string? port = Port();
        if (port == null) return;

        string text = Serialise();
        if (text.Length == 0)
        {
            var answer = MessageBox.Show(this,
                UiText.SlideshowEmptyAsk,
                UiText.AppTitle, MessageBoxButtons.YesNo, MessageBoxIcon.Question);
            if (answer != DialogResult.Yes) return;
        }

        Report("--- writing the sequence ---");
        Background(() =>
        {
            BoardSlots.WriteSequence(port, text, t => BeginInvoke(() => Report(t)),
                                     CancellationToken.None);
            Session.Dirty = true;   // written to the board's RAM, not yet to flash
            // Read back: the board's own form may differ, e.g. 2.0s as 2s.
            string now = BoardSlots.ReadSequence(port, CancellationToken.None);
            BeginInvoke(() =>
            {
                Report("written and read back: " + (now.Length > 0 ? now : "(none)"));
                Parse(now);
                _dirty = false;
                RefreshSteps(_steps.Count > 0 ? 0 : -1);
                Save?.Invoke();
            });
        });
    }
}

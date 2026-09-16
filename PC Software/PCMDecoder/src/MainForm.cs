// The window, laid out as a monitoring instrument: the captured picture
// with a row map beside it (one line per video row, coloured by what
// the decoder made of that row, refreshed every frame), peak meters
// with real ballistics, the format readout and the text channel, the
// counters as rates and totals, a 60 second history of lost and
// degraded rows, and a copyable log. Colours, buttons and the card
// look come from the PAL-sign PC tool's own Theme.cs (linked into this
// project), so the two windows read as one family.
//
// Two clocks: a 40 ms timer (one PAL frame) drives the picture, the
// row map, the meters and the text; a 1 s timer the counters and the
// log line. Frames are converted to RGB on a thread of their own from a
// one-slot mailbox, so a slow UI drops frames instead of stalling the
// capture callback.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Threading;
using System.Windows.Forms;
using PalSign;

namespace PcmDecoder;

public sealed class MainForm : Form
{
    // The PC tool's palette (Theme.cs) under the names this file uses:
    // Carbon is the window ground, Slate a card, Rule a border.
    private static readonly Color Carbon = Theme.Paper;
    private static readonly Color Slate = Theme.Panel;
    private static readonly Color Rule = Theme.Line;
    private static readonly Color Ink = Theme.Ink;
    private static readonly Color Muted = Theme.Muted;
    private static readonly Color Accent = Theme.Accent;
    // The three signal colours, the only ones not in Theme.
    private static readonly Color Good = Color.FromArgb(46, 160, 67);
    private static readonly Color Warn = Color.FromArgb(226, 148, 0);
    private static readonly Color Bad = Color.FromArgb(207, 34, 46);

    private static readonly Font LabelFont = new("Segoe UI", 9f);
    private static readonly Font SmallCaps = new("Segoe UI", 8f, FontStyle.Bold);
    private static readonly Font Readout = new("Consolas", 9.5f);
    private static readonly Font Banner = new("Segoe UI", 16f, FontStyle.Bold);
    private static readonly Font TextChannel = new("Segoe UI", 18f, FontStyle.Bold);
    private static readonly Font LogFont = new("Consolas", 8.5f);

    private readonly ComboBox _source = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 250 };
    private readonly ComboBox _output = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 300 };
    private List<OutputDevice> _outDevices = new();
    // The held audio buffer level: auto is 120 ms on an ASIO route and
    // 200 ms on WaveOut; lower is less latency, and too low shows up
    // as underruns.
    private readonly ComboBox _latency = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 110 };
    // The feed arrives in 40 ms frame bursts, so around a held level
    // the buffer swings 20 ms either way: 30 is the lowest level that
    // never touches empty (confirmed clean in use), 15 pads silence
    // between bursts by arithmetic, and 0 plays every burst straight
    // out. The low steps exist to hear that for oneself.
    private static readonly int?[] LatencySteps = { null, 0, 15, 30, 45, 60, 90, 120, 200 };
    private readonly Button _refresh = Flat("Refresh", 100, false);
    private readonly Button _about = Flat("About", 90, false);
    // The 1 kHz loopback test: the tone out a card of choice, and the
    // frequency the decoder measures coming back, as Hz and as the ppm
    // clock difference between that card and the board's ADC.
    private readonly ComboBox _toneDevice = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 300 };
    private readonly Button _toneBtn = Flat("1 kHz tone", 110, false);
    private readonly Label _toneReadout = new()
        { AutoSize = true, ForeColor = Muted, Font = Readout, Margin = new Padding(10, 8, 0, 0) };
    private TonePlayer? _tonePlayer;
    private string _lastToneDiag = "";
    private readonly Button _start = Flat("Start", 110, true);
    private readonly Label _state = new() { AutoSize = true, ForeColor = Ink, Font = new Font("Segoe UI", 9f, FontStyle.Bold) };
    private readonly PictureBox _picture = new()
        { SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.Black, Width = 720, Height = 576 };
    private readonly RowMap _rowMap = new() { Width = 14, Height = 576 };
    private readonly LevelMeter _meter = new() { Width = 736, Height = 54 };
    private readonly Label _format = new() { AutoSize = true, Font = Banner, ForeColor = Muted };
    private readonly Label _lock = new() { AutoSize = true, Font = LabelFont, ForeColor = Muted };
    private readonly Label _text = new()
        { AutoSize = false, Height = 44, Width = 640, Font = TextChannel, ForeColor = Accent, TextAlign = ContentAlignment.MiddleLeft };
    private readonly History _history = new() { Height = 96, Dock = DockStyle.Top };
    private readonly TextBox _log = new()
        { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Font = LogFont,
          BorderStyle = BorderStyle.None, BackColor = Slate, ForeColor = Ink };
    private readonly Dictionary<string, Label> _values = new();
    private readonly FlowLayoutPanel _groups = new()
        { FlowDirection = FlowDirection.TopDown, WrapContents = true, Dock = DockStyle.Fill, BackColor = Carbon };
    private readonly System.Windows.Forms.Timer _fast = new() { Interval = 40 };
    private readonly System.Windows.Forms.Timer _slow = new() { Interval = 1000 };

    private DecoderSession? _session;
    private Snapshot? _last;
    private readonly List<string> _deckLinks = new();
    private readonly byte[] _health = new byte[576];

    // The preview mailbox: the newest frame waits here for the converter
    // thread; an older one still waiting is simply replaced.
    private FrameEventArgs? _pending;
    private readonly AutoResetEvent _frameReady = new(false);
    private Thread? _converter;
    private volatile bool _converting;

    public MainForm(string deckLinkHint, string soundcardHint)
    {
        Text = "PAL-sign PCM decoder";
        BackColor = Carbon;
        ForeColor = Ink;
        Font = LabelFont;
        ClientSize = new Size(1440, 860);
        MinimumSize = new Size(1440, 860);
        DoubleBuffered = true;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); }
        catch (Exception) { /* the default .NET icon stands */ }

        // Top bar.
        var bar = new FlowLayoutPanel
            { Dock = DockStyle.Top, Height = 48, Padding = new Padding(12, 10, 12, 0), BackColor = Slate, WrapContents = false };
        bar.Controls.Add(Caption("Capture"));
        bar.Controls.Add(_source);
        bar.Controls.Add(_refresh);
        bar.Controls.Add(Caption("Output"));
        bar.Controls.Add(_output);
        _latency.Items.AddRange(new object[] { "Buffer auto", "Buffer 0 ms", "Buffer 15 ms", "Buffer 30 ms", "Buffer 45 ms", "Buffer 60 ms", "Buffer 90 ms", "Buffer 120 ms", "Buffer 200 ms" });
        _latency.SelectedIndex = 0;
        bar.Controls.Add(_latency);
        bar.Controls.Add(_start);
        bar.Controls.Add(_about);
        _state.Margin = new Padding(14, 6, 0, 0);
        bar.Controls.Add(_state);
        _refresh.Margin = new Padding(6, 0, 18, 0);
        _start.Margin = new Padding(12, 0, 0, 0);
        var barLine = new Control { Dock = DockStyle.Top, Height = 1, BackColor = Rule };

        // Left column: picture with the row map at its right edge, the
        // meters under it.
        var left = new TableLayoutPanel
            { Dock = DockStyle.Left, Width = 760, Padding = new Padding(12, 12, 0, 12), BackColor = Carbon, RowCount = 4, ColumnCount = 1 };
        left.RowStyles.Add(new RowStyle(SizeType.Absolute, 580));
        left.RowStyles.Add(new RowStyle(SizeType.Absolute, 60));
        left.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
        left.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        var pictureRow = new System.Windows.Forms.Panel { Width = 736, Height = 576, BackColor = Carbon };
        _picture.Location = new Point(0, 0);
        _rowMap.Location = new Point(722, 0);
        pictureRow.Controls.Add(_picture);
        pictureRow.Controls.Add(_rowMap);
        left.Controls.Add(pictureRow, 0, 0);
        left.Controls.Add(_meter, 0, 1);
        var logHost = new Bordered { Dock = DockStyle.Fill, BackColor = Slate, Padding = new Padding(8), Width = 736 };
        logHost.Dock = DockStyle.None;
        logHost.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left;
        logHost.Width = 736;
        logHost.Height = 150;
        var toneRow = new FlowLayoutPanel { Width = 736, Height = 38, BackColor = Carbon, WrapContents = false, Margin = new Padding(0) };
        toneRow.Controls.Add(Caption("Tone test"));
        toneRow.Controls.Add(_toneDevice);
        _toneBtn.Margin = new Padding(8, 0, 0, 0);
        toneRow.Controls.Add(_toneBtn);
        toneRow.Controls.Add(_toneReadout);
        left.Controls.Add(toneRow, 0, 2);
        _log.Dock = DockStyle.Fill;
        logHost.Controls.Add(_log);
        left.Controls.Add(logHost, 0, 3);

        // Right column: readout, text channel, history, counters.
        var right = new System.Windows.Forms.Panel { Dock = DockStyle.Fill, Padding = new Padding(8, 12, 12, 12), BackColor = Carbon };
        var head = new System.Windows.Forms.Panel { Dock = DockStyle.Top, Height = 100, BackColor = Carbon };
        _format.Location = new Point(0, 0);
        _lock.Location = new Point(2, 36);
        _text.Location = new Point(0, 52);
        head.Controls.Add(_format);
        head.Controls.Add(_lock);
        head.Controls.Add(_text);
        right.Controls.Add(_groups);
        right.Controls.Add(_history);
        right.Controls.Add(head);
        BuildGroups();

        Controls.Add(right);
        Controls.Add(left);
        Controls.Add(barLine);
        Controls.Add(bar);

        _refresh.Click += (_, __) => FillDevices(deckLinkHint, soundcardHint);
        _start.Click += (_, __) => { if (_session == null) StartSession(); else StopSession(); };
        _fast.Tick += (_, __) => FastTick();
        _slow.Tick += (_, __) => SlowTick();
        FormClosing += (_, __) => { StopSession(); _tonePlayer?.Dispose(); _tonePlayer = null; };
        _toneBtn.Click += (_, __) => ToggleTone();
        _about.Click += (_, __) => AboutDialog.ShowAbout(this, "PAL-sign PCM decoder");
        // The version, briefly, at startup; the About button keeps it
        // reachable. Not in the headless --screenshot path, which
        // Show()s the window without a message loop.
        Shown += (_, __) => { if (Application.MessageLoop) AboutDialog.Splash(this, "PAL-sign PCM decoder"); };
        FillDevices(deckLinkHint, soundcardHint);
        Idle();
    }

    // The tone plays independently of a session; the measurement in the
    // readout needs one running, since it lives in the decoded stream.
    private void ToggleTone()
    {
        if (_tonePlayer == null)
        {
            var device = _outDevices[Math.Max(0, _toneDevice.SelectedIndex)];
            // An ASIO driver is single client: a second open against a
            // running session does not just fail (ASE_NoMemory on the
            // GIGAPORT eX), it kills the session's own playback.
            var output = _outDevices[Math.Max(0, _output.SelectedIndex)];
            if (_session != null && device.AsioDriver != null && device.AsioDriver == output.AsioDriver)
            {
                Log("Tone refused: the output already uses this ASIO driver, which allows one " +
                    "client at a time. Use a WaveOut entry for the tone, or another card.");
                return;
            }
            try { _tonePlayer = new TonePlayer(device); }
            catch (Exception ex)
            {
                Log($"Tone failed on {device.Name}: {ex.Message}");
                return;
            }
            _toneBtn.Text = "Stop tone";
            _toneDevice.Enabled = false;
            Log($"1 kHz tone to {device.Name}");
        }
        else
        {
            _tonePlayer.Dispose();
            _tonePlayer = null;
            _toneBtn.Text = "1 kHz tone";
            _toneDevice.Enabled = true;
            Log("Tone stopped");
        }
    }

    private static Button Flat(string text, int width, bool primary)
    {
        var b = new Button { Text = text, Width = width, Height = 32, Font = LabelFont };
        Theme.StyleButton(b, primary);
        return b;
    }

    private static Label Caption(string t) =>
        new() { Text = t, AutoSize = true, ForeColor = Muted, Font = LabelFont, Margin = new Padding(0, 7, 8, 0) };

    // The counter groups: name, then rows of (label, key). The flow
    // wraps into a second column at the window's height.
    private void BuildGroups()
    {
        Group("Signal", ("Lock pixel", "lock"), ("Bit period", "period"), ("Sync shift", "shift"), ("Rows/s ok", "rows"));
        Group("Rows (per s / total)", ("Corrected", "corr"), ("Erasure fixed", "eras"), ("Coarse, 8 bit", "coarse"),
              ("Concealed", "conc"), ("No run-in", "runin"), ("No sync", "sync"));
        Group("Audio", ("Pairs/s", "pairs"), ("Buffer", "buffer"), ("Resample ratio", "ratio"), ("Underruns", "under"),
              ("Overflows", "over"));
        Group("Sony PCM", ("Lines/s ok", "slines"), ("CRC bad", "scrc"), ("Missing", "smiss"), ("Repaired P/Q", "srep"),
              ("Repaired 2", "srep2"), ("Held", "sheld"), ("Data start", "sstart"), ("Control line", "sctrl"));
        Group("Capture", ("Frames", "frames"), ("Dropped", "dropped"), ("No input", "noinput"), ("Handler max", "handler"),
              ("VBI refused", "vbi"));
    }

    private void Group(string title, params (string Label, string Key)[] rows)
    {
        var box = new Card
            { ColumnCount = 2, AutoSize = true, BackColor = Slate, Margin = new Padding(0, 0, 10, 12), Padding = new Padding(13, 0, 13, 10),
              Width = 318 };
        box.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 118));
        box.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 176));
        // The same 30 px bold header CardPanel.cs gives every card in the
        // PC tool, with the rule under it drawn by Card.
        var t = new Label { Text = title, AutoSize = false, Height = Card.HeaderH, ForeColor = Ink,
                            Font = new Font("Segoe UI", 9f, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft,
                            Margin = new Padding(0, 0, 0, 6), Dock = DockStyle.Fill };
        box.Controls.Add(t, 0, 0);
        box.SetColumnSpan(t, 2);
        int r = 1;
        foreach (var (label, key) in rows)
        {
            box.Controls.Add(new Label { Text = label, AutoSize = true, ForeColor = Ink, Font = LabelFont, Margin = new Padding(0, 2, 0, 2) }, 0, r);
            var v = new Label { Text = "—", AutoSize = true, ForeColor = Muted, Font = Readout, Margin = new Padding(0, 2, 0, 2) };
            box.Controls.Add(v, 1, r);
            _values[key] = v;
            r++;
        }
        _groups.Controls.Add(box);
    }

    private void FillDevices(string deckLinkHint, string soundcardHint)
    {
        _deckLinks.Clear();
        _source.Items.Clear();
        try { _deckLinks.AddRange(DeckLinkCapture.ListDevices()); }
        catch (Exception ex) { Log($"DeckLink driver not available: {ex.Message}"); }
        // Generic capture devices (a Dazzle, a USB grabber) join the
        // same list with their "DirectShow: " prefix, which is also how
        // DecoderSession picks the capture path.
        try { _deckLinks.AddRange(DirectShowCapture.ListDevices()); }
        catch (Exception ex) { Log($"DirectShow devices not listed: {ex.Message}"); }
        foreach (var n in _deckLinks) _source.Items.Add(n);
        if (_source.Items.Count == 0) _source.Items.Add("No capture device found");
        _source.SelectedIndex = 0;
        for (int i = 0; i < _deckLinks.Count; i++)
            if (deckLinkHint.Length > 0 && _deckLinks[i].IndexOf(deckLinkHint, StringComparison.OrdinalIgnoreCase) >= 0)
                _source.SelectedIndex = i;

        _output.Items.Clear();
        _outDevices = AudioPlayer.ListDevices();
        foreach (var d in _outDevices) _output.Items.Add(d.Name);
        _output.SelectedIndex = 0;
        int toneSel = Math.Max(0, _toneDevice.SelectedIndex);
        _toneDevice.Items.Clear();
        foreach (var d in _outDevices) _toneDevice.Items.Add(d.Name);
        _toneDevice.SelectedIndex = Math.Min(toneSel, _toneDevice.Items.Count - 1);
        if (soundcardHint.Length > 0)
        {
            try { _output.SelectedIndex = AudioPlayer.ResolveDevice(soundcardHint, _outDevices); }
            catch (ArgumentException ex) { Log(ex.Message); }
        }
        _start.Enabled = _deckLinks.Count > 0;
    }

    private void Idle()
    {
        _state.Text = _deckLinks.Count > 0 ? "Ready" : "Connect a capture device and press Refresh";
        _format.Text = "Not running";
        _format.ForeColor = Muted;
        _lock.Text = "";
        _text.Text = "";
        _meter.Set(0, 0, reset: true);
        _toneReadout.Text = "";
        Array.Clear(_health);
        _rowMap.Set(_health);
    }

    private void StartSession()
    {
        if (_source.SelectedIndex < 0 || _source.SelectedIndex >= _deckLinks.Count) return;
        var soundcard = _outDevices[Math.Max(0, _output.SelectedIndex)];
        try
        {
            _session = new DecoderSession(_deckLinks[_source.SelectedIndex], soundcard,
                                          LatencySteps[Math.Max(0, _latency.SelectedIndex)]);
        }
        catch (Exception ex)
        {
            Log($"Could not start: {ex.Message}");
            return;
        }
        _session.PreviewFrame += QueueFrame;
        _session.LogLine += s => BeginInvoke(() => Log(s));
        _converting = true;
        _converter = new Thread(ConvertLoop) { IsBackground = true, Name = "preview" };
        _converter.Start();
        _history.Clear();
        _last = null;
        _start.Text = "Stop";
        Theme.StyleButton(_start, primary: false);
        _source.Enabled = _output.Enabled = _refresh.Enabled = _latency.Enabled = false;
        _state.Text = $"{_session.DeviceName} → {_output.Text}";
        Log($"Started: {_session.DeviceName} -> {_output.Text}");
        _fast.Start();
        _slow.Start();
    }

    private void StopSession()
    {
        if (_session == null) return;
        _fast.Stop();
        _slow.Stop();
        _session.PreviewFrame -= QueueFrame;
        _converting = false;
        _frameReady.Set();
        _converter?.Join(500);
        _session.Dispose();
        _session = null;
        _start.Text = "Start";
        Theme.StyleButton(_start, primary: true);
        _source.Enabled = _output.Enabled = _refresh.Enabled = _latency.Enabled = true;
        Log("Stopped");
        Idle();
        _state.Text = "Stopped";
    }

    private void Log(string s)
    {
        if (_log.TextLength > 200000) _log.Clear();
        _log.AppendText($"{DateTime.Now:HH:mm:ss}  {s}\r\n");
    }

    // Every frame: meters, row map, text channel.
    private void FastTick()
    {
        if (_session == null) return;
        var (l, r) = _session.TakePeaks();
        _meter.Set(l, r);
        _session.CopyRowHealth(_health);
        _rowMap.Set(_health);
    }

    // Every second: readout, counters, history, log line.
    private void SlowTick()
    {
        if (_session == null) return;
        var s = _session.Snapshot();
        var p = _last;
        double dt = p == null ? 1 : Math.Max(0.001, s.Seconds - p.Seconds);
        _last = s;
        long D(long now, long before) => p == null ? now : now - before;

        long pairsPerSec = (long)(D(s.TotalPairs, p?.TotalPairs ?? 0) / dt);
        bool signal = s.Sony ? D(s.LinesOk, p?.LinesOk ?? 0) > 0 : D(s.RowsOk, p?.RowsOk ?? 0) > 0;
        _format.Text = !signal ? "No signal"
            : s.Sony ? $"Sony PCM  ·  {(s.Sony16 ? "16" : "14")} bit  ·  44.1 kHz"
            : s.Family == 1 ? "Ham PCM LQ  ·  12 bit  ·  32 kHz"
            : s.Family == 2 ? "Ham PCM Voice  ·  8 bit A-law  ·  32 kHz"
            : s.Family == 3 ? "Ham PCM Narrow  ·  mono A-law  ·  16 kHz" : "Ham PCM HQ  ·  14 bit  ·  48 kHz";
        _format.ForeColor = !signal ? Bad : Ink;
        _lock.Text = !signal ? "Nothing decodes: check the composite input and the card on the board"
            : s.Sony ? (s.SonyInPicture ? "Control line in picture" : "Standard placement") + (s.SonyControlSeen ? "  ·  control block seen" : "")
            : $"Lock at pixel {s.LockPixel}  ·  {s.Period:0.00} px/bit  ·  sync shift {s.SyncShift}";
        _text.Text = s.Sony ? "" : s.Text;

        Set("lock", s.Sony ? null : $"{s.LockPixel} px");
        Set("period", s.Sony ? null : $"{s.Period:0.00} px/bit");
        Set("shift", s.Sony ? null : $"{s.SyncShift} px");
        Set("rows", s.Sony ? null : $"{D(s.RowsOk, p?.RowsOk ?? 0) / dt:0}");
        Rate("corr", s.RowsCorrected, p?.RowsCorrected ?? 0, dt, Warn, !s.Sony);
        Rate("eras", s.RowsErasure, p?.RowsErasure ?? 0, dt, Warn, !s.Sony);
        Rate("coarse", s.RowsCoarse, p?.RowsCoarse ?? 0, dt, Warn, !s.Sony);
        Rate("conc", s.RowsConcealed, p?.RowsConcealed ?? 0, dt, Bad, !s.Sony);
        Rate("runin", s.RowsRunIn, p?.RowsRunIn ?? 0, dt, null, !s.Sony);
        Rate("sync", s.RowsSync, p?.RowsSync ?? 0, dt, Warn, !s.Sony);

        Set("slines", s.Sony ? $"{D(s.LinesOk, p?.LinesOk ?? 0) / dt:0}" : null);
        Rate("scrc", s.LinesCrc, p?.LinesCrc ?? 0, dt, Warn, s.Sony);
        Rate("smiss", s.LinesMissing, p?.LinesMissing ?? 0, dt, null, s.Sony);
        Rate("srep", s.BlocksRepaired, p?.BlocksRepaired ?? 0, dt, null, s.Sony);
        Rate("srep2", s.BlocksRepaired2, p?.BlocksRepaired2 ?? 0, dt, Warn, s.Sony);
        Rate("sheld", s.BlocksHeld, p?.BlocksHeld ?? 0, dt, Bad, s.Sony);
        Set("sstart", s.Sony ? $"{s.SonyStart:0.0} px" : null);
        Set("sctrl", s.Sony ? (s.SonyControlSeen ? "seen" : "not seen") : null);

        Set("pairs", $"{pairsPerSec}  of {s.SampleRate}", Math.Abs(pairsPerSec - s.SampleRate) > 200 && signal ? Warn : null);
        Set("buffer", $"{s.BufferedMs:0} ms");
        Set("ratio", $"{s.Ratio:0.00000}");
        Rate("under", s.Underruns, p?.Underruns ?? 0, dt, Bad);
        Rate("over", s.Overflows, p?.Overflows ?? 0, dt, Bad);
        Set("frames", $"{s.Frames}");
        Rate("dropped", s.Dropped, p?.Dropped ?? 0, dt, Bad);
        Rate("noinput", s.NoInput, p?.NoInput ?? 0, dt, Bad);
        Set("handler", $"{s.HandlerMaxMs:0.0} ms", s.HandlerMaxMs > 40 ? Warn : null);
        Set("vbi", s.VbiRefused.Length == 0 ? "none" : s.VbiRefused);

        // The loopback readout: the measured frequency, and how far off
        // 1 kHz it sits in ppm. That deviation is the tone card's clock
        // against the board's ADC clock; the DeckLink never enters into
        // it, the decoder counts samples.
        if (s.ToneHz > 0)
        {
            double ppm = (s.ToneHz / TonePlayer.ToneHz - 1) * 1e6;
            _toneReadout.Text = $"in: {s.ToneHz:0.000} Hz  ({ppm:+0;-0} ppm of 1 kHz)";
            _toneReadout.ForeColor = Math.Abs(ppm) > 10000 ? Warn : Ink;
        }
        else
        {
            _toneReadout.Text = "in: —";
            _toneReadout.ForeColor = Muted;
        }
        // One statistics line per 2 s meter window: mean against median
        // splits clock drift (both move) from dropouts and phase jumps
        // (outliers drag the mean, the median stands).
        if (s.ToneDiag.Length > 0 && s.ToneDiag != _lastToneDiag)
        {
            _lastToneDiag = s.ToneDiag;
            Log($"tone {s.ToneDiag}");
        }

        long lost = s.Sony ? D(s.BlocksHeld, p?.BlocksHeld ?? 0) : D(s.RowsConcealed, p?.RowsConcealed ?? 0);
        long degraded = s.Sony ? D(s.BlocksRepaired + s.BlocksRepaired2, (p?.BlocksRepaired ?? 0) + (p?.BlocksRepaired2 ?? 0))
                               : D(s.RowsCoarse + s.RowsCorrected, (p?.RowsCoarse ?? 0) + (p?.RowsCorrected ?? 0));
        _history.Push((int)lost, (int)degraded);

        // The same line the console used to print, for copying.
        string line = s.Sony
            ? $"SONY{(s.Sony16 ? "16" : "14")} lines ok={s.LinesOk} nosync={s.LinesNoSync} crc={s.LinesCrc} missing={s.LinesMissing} " +
              $"blocks clean={s.BlocksClean} repaired={s.BlocksRepaired} repaired2={s.BlocksRepaired2} held={s.BlocksHeld} " +
              $"ctrl={(s.SonyControlSeen ? "seen" : "none")}{(s.SonyInPicture ? " in-picture" : "")}"
            : $"HAM{(s.Family == 1 ? "-LQ" : s.Family == 2 ? "-VOICE" : s.Family == 3 ? "-NARROW" : "")} rows ok={s.RowsOk} run-in mismatch={s.RowsRunIn} sync mismatch={s.RowsSync} rs-corrected={s.RowsCorrected} " +
              $"(erasures {s.RowsErasure}) coarse={s.RowsCoarse} (tail repaired {s.RowsTailRepaired}) [{s.CoarseRows}] concealed={s.RowsConcealed} period={s.Period:0.00} text=\"{s.Text}\"";
        Log($"{line} pairs/s={pairsPerSec} buffer={s.BufferedMs:0} ms ratio={s.Ratio:0.00000} underruns={s.Underruns} " +
            $"overflows={s.Overflows} frames={s.Frames} dropped={s.Dropped} no-input={s.NoInput} handler max={s.HandlerMaxMs:0.0} ms" +
            (s.ToneHz > 0 ? $" tone={s.ToneHz:0.000} Hz" : ""));
    }

    // null: not applicable in this mode, shown dimmed.
    private void Set(string key, string? value, Color? color = null)
    {
        var l = _values[key];
        l.Text = value ?? "—";
        l.ForeColor = value == null ? Muted : (color ?? Ink);
    }

    // "per second / total", coloured while the rate is nonzero and the
    // counter means trouble.
    private void Rate(string key, long now, long before, double dt, Color? trouble, bool show = true)
    {
        if (!show) { Set(key, null); return; }
        long delta = _last == null ? 0 : now - before;
        double rate = delta / dt;
        Set(key, $"{rate,5:0} / {now}", rate > 0 && trouble != null ? trouble : null);
    }

    // Capture callback thread: drop the frame into the mailbox.
    private void QueueFrame(FrameEventArgs e)
    {
        Interlocked.Exchange(ref _pending, e);
        _frameReady.Set();
    }

    private void ConvertLoop()
    {
        while (_converting)
        {
            _frameReady.WaitOne();
            var e = Interlocked.Exchange(ref _pending, null);
            if (e == null || !_converting) continue;
            var bmp = UyvyToBitmap(e.Pixels, e.Width, e.Height, e.RowBytes);
            if (IsDisposed) { bmp.Dispose(); return; }
            try
            {
                BeginInvoke(() =>
                {
                    var old = _picture.Image;
                    _picture.Image = bmp;
                    old?.Dispose();
                });
            }
            catch (InvalidOperationException) { bmp.Dispose(); }
        }
    }

    // BT.601, the same matrix gfx.h's yccFromRgb() uses (inverted): full
    // fidelity is not the point here, just a recognisable picture.
    private static Bitmap UyvyToBitmap(byte[] pixels, int width, int height, int rowBytes)
    {
        var bmp = new Bitmap(width, height, PixelFormat.Format24bppRgb);
        var data = bmp.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
        unsafe
        {
            for (int y = 0; y < height; y++)
            {
                byte* dst = (byte*)data.Scan0 + y * data.Stride;
                int rowOff = y * rowBytes;
                for (int x = 0; x < width; x += 2)
                {
                    int quad = rowOff + (x / 2) * 4;
                    int cb = pixels[quad] - 128;
                    int y0 = pixels[quad + 1] - 16;
                    int cr = pixels[quad + 2] - 128;
                    int y1 = pixels[quad + 3] - 16;
                    WritePixel(dst, x * 3, y0, cb, cr);
                    if (x + 1 < width) WritePixel(dst, (x + 1) * 3, y1, cb, cr);
                }
            }
        }
        bmp.UnlockBits(data);
        return bmp;
    }

    private static unsafe void WritePixel(byte* dst, int off, int y, int cb, int cr)
    {
        int r = Clamp((298 * y + 409 * cr + 128) >> 8);
        int g = Clamp((298 * y - 100 * cb - 208 * cr + 128) >> 8);
        int b = Clamp((298 * y + 516 * cb + 128) >> 8);
        dst[off] = (byte)b;
        dst[off + 1] = (byte)g;
        dst[off + 2] = (byte)r;
    }

    private static int Clamp(int v) => v < 0 ? 0 : (v > 255 ? 255 : v);

    private sealed class Bordered : System.Windows.Forms.Panel
    {
        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            using var pen = new Pen(Rule);
            e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
        }
    }

    // A white panel with the PC tool's card border and header rule.
    private sealed class Card : TableLayoutPanel
    {
        public const int HeaderH = 30;
        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            using var pen = new Pen(Rule);
            e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
            e.Graphics.DrawLine(pen, 0, HeaderH, Width - 1, HeaderH);
        }
    }

    // One line per video row of the captured frame, in capture row
    // order (top to bottom as seen), coloured by what the decoder made
    // of it: green decoded, amber degraded (8 bit or repaired), red
    // lost, dark for rows that carry nothing (the text band, the two
    // rows past the picture).
    private sealed class RowMap : Control
    {
        private readonly byte[] _rows = new byte[576];
        public RowMap() { DoubleBuffered = true; BackColor = Carbon; }
        public void Set(byte[] rows) { Array.Copy(rows, _rows, 576); Invalidate(); }
        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var g = e.Graphics;
            using var none = new SolidBrush(Rule);
            using var ok = new SolidBrush(Good);
            using var deg = new SolidBrush(Warn);
            using var lost = new SolidBrush(Bad);
            double h = Height / 576.0;
            for (int y = 0; y < 576; y++)
            {
                var b = _rows[y] switch { 1 => ok, 2 => deg, 3 => lost, _ => none };
                int y0 = (int)(y * h), y1 = (int)((y + 1) * h);
                g.FillRectangle(b, 2, y0, Width - 4, Math.Max(1, y1 - y0));
            }
        }
    }

    // Two peak-programme style bars: instant attack, 20 dB/s release
    // (0.8 dB a frame at 40 ms), a peak-hold tick that falls after a
    // second, the scale in dBFS.
    private sealed class LevelMeter : Control
    {
        private double _l = -60, _r = -60, _hl = -60, _hr = -60;
        private int _holdL, _holdR;
        public LevelMeter() { DoubleBuffered = true; BackColor = Carbon; }
        public void Set(int peakL, int peakR, bool reset = false)
        {
            if (reset) { _l = _r = _hl = _hr = -60; Invalidate(); return; }
            _l = Math.Max(Db(peakL), _l - 0.8);
            _r = Math.Max(Db(peakR), _r - 0.8);
            if (_l >= _hl) { _hl = _l; _holdL = 25; } else if (--_holdL < 0) _hl = Math.Max(_l, _hl - 1.5);
            if (_r >= _hr) { _hr = _r; _holdR = 25; } else if (--_holdR < 0) _hr = Math.Max(_r, _hr - 1.5);
            Invalidate();
        }
        private static double Db(int peak) => peak <= 0 ? -60 : Math.Max(-60, 20 * Math.Log10(peak / 32767.0));
        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.None;
            using var muted = new SolidBrush(Muted);
            using var ink = new SolidBrush(Ink);
            int x0 = 24, w = Width - x0 - 78, h = 12;
            Bar(g, x0, 4, w, h, _l, _hl, "L", muted, ink);
            Bar(g, x0, 20, w, h, _r, _hr, "R", muted, ink);
            using var pen = new Pen(Rule);
            for (int db = -60; db <= 0; db += 6)
            {
                int x = x0 + (int)(w * (db + 60) / 60.0);
                g.DrawLine(pen, x, 34, x, 37);
                if (db % 12 == 0 || db == -6)
                {
                    string t = db == 0 ? "0 dBFS" : db.ToString();
                    var sz = g.MeasureString(t, SmallCaps);
                    g.DrawString(t, SmallCaps, muted, x - sz.Width / 2, 38);
                }
            }
        }
        private static void Bar(Graphics g, int x, int y, int w, int h, double db, double hold, string name, Brush muted, Brush ink)
        {
            g.DrawString(name, SmallCaps, muted, 4, y - 1);
            using var back = new SolidBrush(Slate);
            g.FillRectangle(back, x, y, w, h);
            int fill = (int)(w * (db + 60) / 60.0);
            using var green = new SolidBrush(Good);
            using var amber = new SolidBrush(Warn);
            using var red = new SolidBrush(Bad);
            int amberAt = (int)(w * (60 - 12) / 60.0), redAt = (int)(w * (60 - 3) / 60.0);
            g.FillRectangle(green, x, y, Math.Min(fill, amberAt), h);
            if (fill > amberAt) g.FillRectangle(amber, x + amberAt, y, Math.Min(fill, redAt) - amberAt, h);
            if (fill > redAt) g.FillRectangle(red, x + redAt, y, fill - redAt, h);
            int hx = x + (int)(w * (hold + 60) / 60.0);
            g.FillRectangle(ink, Math.Min(hx, x + w - 2), y, 2, h);
            g.DrawString($"{hold,4:0} dB", Readout, ink, x + w + 6, y - 2);
        }
    }

    // Sixty seconds of rows lost (red) and degraded (amber) a second,
    // newest at the right, scaled to the largest value in view.
    private sealed class History : Control
    {
        private readonly int[] _lost = new int[60], _degraded = new int[60];
        public History() { DoubleBuffered = true; BackColor = Slate; }
        protected override void OnPaintBackground(PaintEventArgs e)
        {
            base.OnPaintBackground(e);
            using var pen = new Pen(Rule);
            e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
        }
        public void Clear() { Array.Clear(_lost); Array.Clear(_degraded); Invalidate(); }
        public void Push(int lost, int degraded)
        {
            Array.Copy(_lost, 1, _lost, 0, 59);
            Array.Copy(_degraded, 1, _degraded, 0, 59);
            _lost[59] = lost; _degraded[59] = degraded;
            Invalidate();
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var g = e.Graphics;
            using var muted = new SolidBrush(Muted);
            g.DrawString("Rows lost (red) and degraded (amber), last 60 s", SmallCaps, muted, 12, 8);
            int top = 28, bottom = Height - 10, x0 = 12, w = Width - 24;
            using var pen = new Pen(Rule);
            g.DrawLine(pen, x0, bottom, x0 + w, bottom);
            int max = 1;
            for (int i = 0; i < 60; i++) max = Math.Max(max, Math.Max(_lost[i], _degraded[i]));
            using var red = new SolidBrush(Bad);
            using var amber = new SolidBrush(Warn);
            double bw = w / 60.0;
            for (int i = 0; i < 60; i++)
            {
                int x = x0 + (int)(i * bw);
                int hd = (int)((bottom - top) * _degraded[i] / (double)max);
                int hl = (int)((bottom - top) * _lost[i] / (double)max);
                if (hd > 0) g.FillRectangle(amber, x, bottom - hd, (int)bw - 1, hd);
                if (hl > 0) g.FillRectangle(red, x, bottom - hl, (int)bw - 1, hl);
            }
            string peak = $"peak {max}/s";
            var sz = g.MeasureString(peak, SmallCaps);
            g.DrawString(peak, SmallCaps, muted, x0 + w - sz.Width, 8);
        }
    }
}

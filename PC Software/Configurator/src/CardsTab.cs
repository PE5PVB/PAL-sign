// Tab page: every test card in a list, with what the board keeps for that
// one card next to it.
//
// A setting is sent to the board at once and only written to flash on
// "Save to flash". The slider is live: the port stays open while
// dragging (opening and closing costs tens of milliseconds) and the
// queue keeps only the newest value per field, by name, so a value set
// again before it went over replaces the one waiting.
//
// WinForms decides who wins a dock conflict by the order of
// Controls.Add; layout code in this program relies on that order.
//
// Split across six files as a C# partial class.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Threading;
using System.Windows.Forms;

namespace PalSign;

// DoubleBuffered manually: WinForms keeps Panel.DoubleBuffered
// protected, and the preview is redrawn 25 times a second.
internal sealed class SteadyPanel : Panel
{
    public SteadyPanel()
    {
        DoubleBuffered = true;
        SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint |
                 ControlStyles.UserPaint, true);
    }
}

public partial class CardsTab : TabPage
{
    private readonly Func<string?> _port;

    // Band height of the two raster ticker faces (PM5544, PM8546):
    // margin = max(pad, how far the highest outlier reaches above the
    // capital, how far the deepest reaches below), height = 2 * margin
    // + cap, rounded up to even.
    private static readonly int[] BandHeight = { 38, 30 };

    // The same sum for Inter and Doto, one row per face, one column per
    // size (extra small/small/medium/large/extra large, ticker.cpp's
    // own five fixed bakes): {50,68,88,104,134} for Inter, {52,62,78,
    // 92,116} for Doto. Fixed, not computed: each is its own separate
    // bake, not a scaled derivative of another, so there is no single
    // formula left to run here the way BandOf() once did for a size
    // slider.
    private static readonly int[][] BigBandHeight =
        { new[] { 50, 68, 88, 104, 134 }, new[] { 52, 62, 78, 92, 116 } };
    private const int ScreenH = 576;

    private readonly ListBox _list = new()
    {
        Dock = DockStyle.Fill,
        DrawMode = DrawMode.OwnerDrawFixed,
        ItemHeight = 20,
        IntegralHeight = false,
        Font = new Font(FontFamily.GenericMonospace, 9f),
    };

    // Ticker face has no swatch; its right margin matches the swatch
    // column width so the three boxes stay equally wide.
    private readonly ComboBox _ticker = new()
        { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
    private readonly ComboBox _aspect = new()
        { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
    private readonly ComboBox _insert = new()
        { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
    private readonly ComboBox _font = new()
        { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill,
          Margin = new Padding(3, 3, 37, 3) };

    // The card's own lettering: identification, clock, inserts. The
    // ticker has its own face box among the ticker rows.
    private readonly ComboBox _textFace = new()
        { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill,
          Margin = new Padding(3, 3, 37, 3) };
    private readonly CheckBox _showId = new()
        { Text = UiText.ShowFirstLine, AutoSize = true };

    // Three of the identification cards draw only the first line; the
    // board says which through hassub=.
    private readonly CheckBox _showSub = new()
        { Text = UiText.ShowSecondLine, AutoSize = true };

    // Only a card with a line box gets this switch; the board says
    // which through hasline=.
    private readonly CheckBox _moving = new()
        { Text = UiText.MovingLineSwitch, AutoSize = true, Visible = false };

    // Always visible: switched off, a card is skipped by p, P, the
    // knob and the slideshow fallback on the board.
    private readonly CheckBox _enabled = new()
        { Text = UiText.CardEnabledSwitch, AutoSize = true };

    // Per-card switch; the board says which card owns which.
    private readonly CheckBox _bars = new()
        { Text = UiText.ChromaBars, AutoSize = true, Visible = false };
    private readonly CheckBox _ap2 = new()
        { Text = UiText.Ap2Variant, AutoSize = true, Visible = false };

    // BBC Test Card F and W only (Own == 4); unlike bars/ap2 this one
    // really is per card (see CardInfo.CustomPhoto), so it goes through
    // Push() like showid/moving, not PushShared().
    private readonly CheckBox _customPhoto = new()
        { Text = UiText.CustomPhotoSwitch, AutoSize = true, Visible = false };
    private readonly Button _uploadPortrait = new()
        { Text = UiText.UploadPortrait, AutoSize = true, Visible = false };

    // Sony PCM only (Own == 5): the two bits of the card's option byte,
    // sent together as one "option" value through Push() like customphoto.
    // That card has no ticker and no aspect ratio either; those rows are
    // hidden for it (see FillFromCard()).
    private readonly ComboBox _pcmBits = new()
        { DropDownStyle = ComboBoxStyle.DropDownList };
    private TableLayoutPanel _pcmBitsRow = null!;
    private readonly CheckBox _pcmEmphasis = new()
        { Text = UiText.PcmPreEmphasis, AutoSize = true, Visible = false };
    private readonly CheckBox _pcmCtrlPic = new()
        { Text = UiText.PcmCtrlInPicture, AutoSize = true, Visible = false };
    // Ham PCM: the text channel, one setting on the board.
    private readonly TextBox _pcmText = new()
        { Width = 120, MaxLength = 10, CharacterCasing = CharacterCasing.Upper };
    private TableLayoutPanel _pcmTextRow = null!;
    // Ham PCM: the quality, the card's option bits 1..0.
    private readonly ComboBox _hamQuality = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private TableLayoutPanel _hamQualityRow = null!;

    private void PushPcmOption() =>
        Push("option", (_pcmBits.SelectedIndex == 1 ? 1 : 0) | (_pcmEmphasis.Checked ? 2 : 0) |
                       (_pcmCtrlPic.Checked ? 4 : 0));

    // The rows a raw-signal card (Sony PCM) does without; label and
    // control hidden together so the table keeps its columns.
    private Label _aspectLabel = null!, _tickerLabel = null!, _tickerFontLabel = null!,
        _tickerSizeLabel = null!, _yLabel = null!, _speedLabel = null!;
    private Control _tickHost = null!, _yRow = null!, _speedRow = null!;

    // The contest page only; one setting each on the board.
    private readonly TextBox _contest1 = new() { Width = 185, MaxLength = 31 };
    private readonly TextBox _contest2 = new() { Width = 185, MaxLength = 31 };
    private readonly FixedDigits _contestNr = new()
        { Minimum = 0, Maximum = 9999, Width = 80 };
    private readonly TableLayoutPanel _contest = new()
    {
        ColumnCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
        Margin = new Padding(0, 4, 0, 0), Visible = false,
    };

    // The reason for a switch goes in its tooltip: the options column
    // is too narrow for a sentence.
    private readonly ToolTip _why = new() { AutoPopDelay = 15000 };

    // Only three cards draw insert boxes; the host carries the box and
    // its colour swatch as one.
    private Label _insertLabel = null!;
    private TableLayoutPanel _insHost = null!;

    // Left out where a card projects no text at all. Kept as a pair
    // with _textFace: hiding one cell of a TableLayoutPanel row moves
    // the cells after it.
    private Label _textFaceLabel = null!;

    // A TableLayoutPanel hands out cells in Controls.Add order and
    // skips invisible ones, so a per-card option needs a holder that is
    // always visible itself, or the cells after it shift when it is
    // hidden. One column: the swatch has to reach the right edge, which
    // a flow panel would not stretch to.
    private readonly TableLayoutPanel _own = new()
    {
        ColumnCount = 1,
        AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
        Dock = DockStyle.Fill, Margin = new Padding(0),
    };
    // Flat, because a themed button paints its own face over BackColor.
    private readonly Button _colTicker = Swatch();
    private readonly Button _colId = Swatch();
    private readonly Button _colSub = Swatch();
    private readonly Button _colIns = Swatch();

    // Swatch beside its switch, in an AutoSize column at the right
    // edge, in line with the other swatches.
    private readonly TableLayoutPanel _idRow = OptionRow();
    private readonly TableLayoutPanel _subRow = OptionRow();

    private readonly AccentSlider _y = new()
    {
        Minimum = 0, Maximum = ScreenH, TickFrequency = 48, SmallChange = 2, LargeChange = 16,
        Dock = DockStyle.Fill, Height = 34,
    };
    private readonly Label _yValue = new() { AutoSize = true, Margin = new Padding(6, 8, 0, 0) };

    // Speed and direction in one slider: left of the middle the text
    // walks left, right of it right. Zero does not exist as a value
    // (the off switch is for that); the slider hops over it and the
    // board snaps a stored zero away too. Ladder, not a free number;
    // see cfgCardTickerSpeed on the board.
    private readonly AccentSlider _speed = new()
    {
        Minimum = -8, Maximum = 8, TickFrequency = 1, SmallChange = 1, LargeChange = 2,
        Dock = DockStyle.Fill, Height = 34,
    };
    private readonly Label _speedValue = new()
        { AutoSize = true, Margin = new Padding(6, 8, 0, 0) };
    private int _speedPrev = -4;   // where the hop over zero comes from

    // Which of the five fixed sizes the big faces (Inter, Doto) draw
    // at; only meaningful with one of those two as the ticker font, see
    // FillFromCard(). A box, not a slider: five named, individually
    // baked sizes (a size slider that scaled at render time was tried
    // and reverted), not a range. Same right
    // margin as _font, right below it: the two line up at the same
    // width.
    private readonly ComboBox _size = new()
        { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill,
          Margin = new Padding(3, 3, 37, 3) };
    private readonly SteadyPanel _preview = new()
        { Dock = DockStyle.Fill, MinimumSize = new Size(0, 150) };

    // Connect and Save live in the bar; this row is only what needs a
    // card in front of you.
    private readonly Button _upload = new()
        { Text = UiText.UploadPicture, AutoSize = true, Margin = new Padding(0, 0, 8, 0), Visible = false };
    private readonly Button _empty = new()
        { Text = UiText.EmptyThisSlot, AutoSize = true, Margin = new Padding(0, 0, 8, 0), Visible = false };
    private readonly Button _saveAs = new()
        { Text = UiText.SaveAsPicture, AutoSize = true, Anchor = AnchorStyles.None,
          Margin = new Padding(0) };

    /// <summary>Where the status line goes. Null-safe, so the page also
    /// works without a window around it, as in the layout check.</summary>
    public Action<string>? Status;
    // The read/write progress lives in a modal window; see
    // ProgressOpen() in CardsTab.Board.cs and ProgressDialog.cs.

    private BoardSettings? _cfg;
    private bool _loading;      // set while the controls are being filled
    private bool _busy;

    private readonly Dictionary<string, string> _pending = new();
    private System.IO.Ports.SerialPort? _live;
    private int _quiet;         // ticks with nothing to send
    // 40 ms: the 25 Hz the board moves its band at, two samples a
    // picture.
    private int _scroll;
    private int _scrollPhase;   // ticks waited on the slow rungs of the ladder
    private readonly System.Windows.Forms.Timer _tick = new() { Interval = 40 };

    // Off the board, kept on disk between sessions; see CardCache.
    private readonly Dictionary<int, Image> _picture = new();
    private readonly Dictionary<int, CardBoxes> _boxes = new();

    // Bytes just uploaded, by card number, so FetchPictures can use them
    // instead of fetching back over the port. Written before the fetch
    // worker starts, taken once on that worker.
    private readonly Dictionary<int, byte[]> _fresh = new();

    // Cards whose kept picture may no longer match the board, because a
    // shared setting touches every card at once. Only the card on
    // screen is fetched right away; the rest wait until selected.
    private readonly HashSet<int> _stale = new();
    private bool _refreshDue;   // a dump is owed, once the board is free
    private bool _sharedChanged;   // for the save
    private bool _askCrc;          // for the card being looked at; see AskCardCrc()
    private int _wanted;        // the card the refresh is really after

    // Set from SlideshowTab (see MainWindow's wiring), so the list and
    // preview here can follow a slideshow the board is stepping through
    // on its own; see AskCurrentCard().
    private bool _slideshowRunning;
    private int _slidePollTicks;   // idle ticks since the last ask

    // The board's own clock, not this computer's: no battery, restarts
    // at the time in config.h after a power cut. Read once at Connect
    // and counted on from there.
    private DateTime _boardClock;
    private DateTime _readAt;
    private int _dateFormat;

    // The faces the board draws insert boxes with, so the clock in the
    // preview matches; keyed by font index, per card.
    private Dictionary<int, BoardFont> _faces = new();

    /// <summary>What Connect read, passed on so nothing else has to ask
    /// the board again.</summary>
    public Action<BoardSettings>? Settings;

    /// <summary>Whether the window currently has an answer from the
    /// board, so the bar can offer Connect or Disconnect.</summary>
    public Action<bool>? Live;

    public CardsTab(Func<string?> port)
    {
        _port = port;
        Text = UiText.TabCards;
        BackColor = Theme.Paper;
        Padding = new Padding(12);
        Theme.StyleButton(_upload, primary: true);
        Theme.StyleButton(_empty);
        Theme.StyleButton(_saveAs);
        Theme.StyleButton(_uploadPortrait, primary: true);

        _list.DrawItem += DrawCard;
        _list.SelectedIndexChanged += (_, __) => FillFromCard();
        // Double click is the only way to put a card on the television:
        // selecting only shows its settings. An empty or disabled card
        // is refused, matching the board's own stepping.
        _list.DoubleClick += (_, __) =>
        {
            var c = Current;
            if (c == null) return;
            if (c.Photo && c.Name == "Empty slot") { Say(UiText.SlotIsEmpty); return; }
            if (!c.Enabled) { Say(UiText.CardIsOff); return; }
            Run(() => ShowOnBoard(c.Number));
        };
        _preview.Paint += DrawPreview;

        _ticker.SelectedIndexChanged += (_, __) => Push("ticker", _ticker.SelectedIndex);
        _textFace.SelectedIndexChanged += (_, __) => Push("textfont", _textFace.SelectedIndex);
        _textFace.Items.Add(UiText.FacePm5544);
        _textFace.Items.Add(UiText.FacePm8546);
        _textFace.Items.Add(UiText.FaceInter);
        // Index 3, the same order FONTS[] in font_data.h keeps them in:
        // SelectedIndex goes to the board as the raw font number.
        _textFace.Items.Add(UiText.FaceSquare);
        _size.SelectedIndexChanged += (_, __) => Push("tickerscale", _size.SelectedIndex);
        _size.Items.Add(UiText.TickerSizeXSmall);
        _size.Items.Add(UiText.TickerSizeSmall);
        _size.Items.Add(UiText.TickerSizeMedium);
        _size.Items.Add(UiText.TickerSizeLarge);
        _size.Items.Add(UiText.TickerSizeXLarge);
        _aspect.SelectedIndexChanged += (_, __) => Push("aspect", _aspect.SelectedIndex);
        _insert.SelectedIndexChanged += (_, __) => Push("insert", _insert.SelectedIndex);
        _font.SelectedIndexChanged += (_, __) => Push("tickerfont", _font.SelectedIndex);
        _showId.CheckedChanged += (_, __) => Push("showid", _showId.Checked ? 1 : 0);
        _showSub.CheckedChanged += (_, __) => Push("showsub", _showSub.Checked ? 1 : 0);
        _moving.CheckedChanged += (_, __) => Push("moving", _moving.Checked ? 1 : 0);
        _why.SetToolTip(_moving, UiText.TipMovingLine);
        _enabled.CheckedChanged += (_, __) => Push("enabled", _enabled.Checked ? 1 : 0);
        _why.SetToolTip(_enabled, UiText.TipCardEnabled);
        _why.SetToolTip(_list, UiText.TipCardList);
        _why.SetToolTip(_bars, UiText.TipChromaBars);
        _why.SetToolTip(_ap2, UiText.TipAp2);
        _why.SetToolTip(_customPhoto, UiText.TipCustomPhoto);
        _bars.CheckedChanged += (_, __) => PushShared("g924bars", _bars.Checked ? 1 : 0);
        _ap2.CheckedChanged += (_, __) => PushShared("tcgap2", _ap2.Checked ? 1 : 0);
        _customPhoto.CheckedChanged += (_, __) => Push("customphoto", _customPhoto.Checked ? 1 : 0);
        _why.SetToolTip(_pcmBits, UiText.TipPcm16Bit);
        _why.SetToolTip(_pcmEmphasis, UiText.TipPcmPreEmphasis);
        _why.SetToolTip(_pcmCtrlPic, UiText.TipPcmCtrlInPicture);
        _pcmBits.Items.Add(UiText.Pcm14Bit);
        _pcmBits.Items.Add(UiText.Pcm16Bit);
        _pcmBits.SelectedIndexChanged += (_, __) => PushPcmOption();
        _pcmEmphasis.CheckedChanged += (_, __) => PushPcmOption();
        _pcmCtrlPic.CheckedChanged += (_, __) => PushPcmOption();
        _uploadPortrait.Click += (_, __) => UploadPortrait();
        // Contest lines take only what the board's faces can draw; see BoardText.
        BoardText.Guard(_contest1);
        BoardText.Guard(_contest2);
        SendOnLeave(_contest1, "contest1");
        SendOnLeave(_contest2, "contest2");
        SendOnLeave(_pcmText, "pcmtext");
        _why.SetToolTip(_pcmText, UiText.TipPcmText);
        _why.SetToolTip(_hamQuality, UiText.TipHamQuality);
        _hamQuality.Items.Add(UiText.HamHq);
        _hamQuality.Items.Add(UiText.HamLq);
        _hamQuality.Items.Add(UiText.HamVoice);
        _hamQuality.Items.Add(UiText.HamNarrow);
        _hamQuality.SelectedIndexChanged += (_, __) => Push("option", _hamQuality.SelectedIndex);
        WireSwatch(_colTicker, "tickercolor", UiText.TipTickerColor);
        WireSwatch(_colId, "idcolor", UiText.TipIdColor);
        WireSwatch(_colSub, "subcolor", UiText.TipSubColor);
        WireSwatch(_colIns, "inscolor", UiText.TipInsColor);
        _contestNr.ValueChanged += (_, __) =>
            PushShared("contestnr", (int)_contestNr.Value);

        _y.ValueChanged += (_, __) =>
        {
            SnapY();
            ShowY();
            Push("tickery", _y.Value);
        };
        _speed.ValueChanged += (_, __) =>
        {
            if (_speed.Value == 0)
            {
                _speed.Value = _speedPrev < 0 ? 1 : -1;   // comes back here once
                return;
            }
            _speedPrev = _speed.Value;
            ShowSpeed();
            Push("tickerspeed", _speed.Value);
        };
        _tick.Tick += (_, __) => { Drain(); RefreshIfDue(); Roll(); };
        _tick.Start();

        _upload.Click += (_, __) => UploadPicture();
        _empty.Click += (_, __) => EmptySlot();
        _saveAs.Click += (_, __) => SavePicture();

        Controls.Add(BuildLayout());
        SetEnabled(false);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _tick.Stop();
            DropLive();
            foreach (var img in _picture.Values) img.Dispose();
            _picture.Clear();
        }
        base.Dispose(disposing);
    }

    private void Say(string s) => Status?.Invoke(s);

    // On a thread: the board gives up sending a card if this side stops
    // reading for a second, and a message pump in the read loop is
    // enough to trigger that. Everything the work does to the window
    // goes back through Post().
    private void Run(Action what)
    {
        if (_busy) { Say(UiText.StillBusy); return; }
        _busy = true;
        // Opens a port of its own, so the one held for live changes has
        // to be let go first. What is still queued is kept; it is
        // dropped only in Adopt(), where the whole model is replaced.
        DropLive();
        Cursor = Cursors.AppStarting;

        var worker = new Thread(() =>
        {
            try { what(); }
            catch (Exception ex) { Post(() => Say(ex.Message)); }
            finally
            {
                Post(() =>
                {
                    Cursor = Cursors.Default;
                    _busy = false;
                });
            }
        })
        { IsBackground = true };
        worker.Start();
    }
}

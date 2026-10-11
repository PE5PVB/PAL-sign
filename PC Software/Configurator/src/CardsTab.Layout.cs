// CardsTab, the part that builds the visual tree.
using System.Drawing;
using System.Windows.Forms;

namespace PalSign;

public partial class CardsTab
{
    // 30 wide plus margin, so the longest switch text and the swatch
    // both fit inside the options column.
    private static Button Swatch()
    {
        var b = new Button
        {
            Width = 30, Height = 23, FlatStyle = FlatStyle.Flat, BackColor = Color.White,
            Margin = new Padding(4, 3, 0, 0), Cursor = Cursors.Hand,
        };
        b.FlatAppearance.BorderColor = Theme.Line;
        return b;
    }

    private static TableLayoutPanel OptionRow()
    {
        var t = new TableLayoutPanel
        {
            ColumnCount = 2, Dock = DockStyle.Fill,
            AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0),
        };
        t.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        t.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        t.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        return t;
    }

    /// <summary>For the layout check only: shows every per-card switch,
    /// so the check measures a page that is otherwise never fully
    /// visible.</summary>
    public void ShowEveryOption()
    {
        _idRow.Visible = true;
        _subRow.Visible = true;
        _moving.Visible = true;
        _insertLabel.Visible = true;
        _insHost.Visible = true;
        _textFaceLabel.Visible = true;
        _textFace.Visible = true;
        _bars.Visible = true;
        _ap2.Visible = true;
        _contest.Visible = true;
        _customPhoto.Visible = true;
        _uploadPortrait.Visible = true;
        _pcmBitsRow.Visible = true;
        _pcmEmphasis.Visible = true;
        _pcmCtrlPic.Visible = true;
        _pcmTextRow.Visible = true;
        _legacyTextRow.Visible = true;
        _hamQualityRow.Visible = true;
        _clockTextRow.Visible = true;
        _clockColorRow.Visible = true;
        ShowRasterRows(true);
    }

    // The aspect ratio and the ticker rows: everything a raw-signal card
    // (Sony PCM, own=5) has no use for.
    private void ShowRasterRows(bool on)
    {
        _aspectLabel.Visible = on; _aspect.Visible = on;
        _tickerLabel.Visible = on; _tickHost.Visible = on;
        _tickerFontLabel.Visible = on; _font.Visible = on;
        _tickerSizeLabel.Visible = on; _size.Visible = on;
        _yLabel.Visible = on; _yRow.Visible = on;
        _speedLabel.Visible = on; _speedRow.Visible = on;
    }

    /// <summary>For the layout check only: hides everything that can be
    /// left out, since that is where TableLayoutPanel cells move.</summary>
    public void HideEveryOption()
    {
        _idRow.Visible = false;
        _subRow.Visible = false;
        _moving.Visible = false;
        _textFaceLabel.Visible = false;
        _textFace.Visible = false;
        _bars.Visible = false;
        _ap2.Visible = false;
        _contest.Visible = false;
        _customPhoto.Visible = false;
        _uploadPortrait.Visible = false;
        _pcmBitsRow.Visible = false;
        _pcmEmphasis.Visible = false;
        _pcmCtrlPic.Visible = false;
        _pcmTextRow.Visible = false;
        _legacyTextRow.Visible = false;
        _hamQualityRow.Visible = false;
        _clockTextRow.Visible = false;
        _clockColorRow.Visible = false;
        _insertLabel.Visible = false;
        _insHost.Visible = false;
    }

    // Kept for EqualPreviewSize(): the grid and its options cell.
    private TableLayoutPanel _root = null!;
    private Control _options = null!;

    private Control BuildLayout()
    {
        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 3,
        };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 260));
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        // The preview row takes the slack, not the options row.
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        root.Controls.Add(_list, 0, 0);
        root.SetRowSpan(_list, 2);
        _options = BuildOptions();
        root.Controls.Add(_options, 1, 0);
        _root = root;
        root.Controls.Add(BuildPreviewHolder(), 1, 1);

        return root;
    }

    private Control BuildOptions()
    {
        var t = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, ColumnCount = 2, AutoSize = true,
            Padding = new Padding(12, 0, 0, 0),
        };
        t.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 130));
        t.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

        Label Row(string label, Control c)
        {
            var l = new Label { Text = label, AutoSize = true, Margin = new Padding(0, 6, 8, 0) };
            t.Controls.Add(l);
            t.Controls.Add(c);
            t.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            return l;
        }

        // Always visible, own row: whether the card takes part comes
        // before how it is dressed.
        t.Controls.Add(new Label { Text = "", AutoSize = true });
        t.Controls.Add(_enabled);
        t.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        _aspectLabel = Row("Aspect / WSS", _aspect);
        _textFaceLabel = new Label
            { Text = "Text font", AutoSize = true, Margin = new Padding(0, 6, 8, 0) };
        t.Controls.Add(_textFaceLabel);
        t.Controls.Add(_textFace);
        t.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var tickHost = OptionRow();
        tickHost.Controls.Add(_ticker);
        tickHost.Controls.Add(_colTicker);
        _tickHost = tickHost;
        _tickerLabel = Row("Ticker", tickHost);
        _tickerFontLabel = Row("Ticker font", _font);
        // Greyed, not hidden, when the ticker font is not Inter or Doto
        // (FillFromCard()): a size only those two have. Right under its
        // own font row, and the same width (the same right margin,
        // matching the swatch column _font already leaves room for).
        _tickerSizeLabel = Row("Ticker font size", _size);

        var yRow = new TableLayoutPanel
            { ColumnCount = 2, Dock = DockStyle.Fill, AutoSize = true, Margin = new Padding(0) };
        yRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        yRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
        yRow.Controls.Add(_y, 0, 0);
        yRow.Controls.Add(_yValue, 1, 0);
        _yRow = yRow;
        _yLabel = Row("Band at row", yRow);

        var speedRow = new TableLayoutPanel
            { ColumnCount = 2, Dock = DockStyle.Fill, AutoSize = true, Margin = new Padding(0) };
        speedRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        speedRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
        speedRow.Controls.Add(_speed, 0, 0);
        speedRow.Controls.Add(_speedValue, 1, 0);
        _speedRow = speedRow;
        _speedLabel = Row("Ticker speed", speedRow);
        // Label kept as a pair with the box: hiding only one would move
        // every cell after it, hiding both keeps the columns as they are.
        _insertLabel = new Label
            { Text = UiText.InsertBoxesLabel, AutoSize = true, Margin = new Padding(0, 6, 8, 0) };
        t.Controls.Add(_insertLabel);
        _insHost = new TableLayoutPanel
        {
            ColumnCount = 2, Dock = DockStyle.Fill,
            AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0),
        };
        _insHost.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        _insHost.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _insHost.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        _insHost.Controls.Add(_insert);
        _insHost.Controls.Add(_colIns);
        t.Controls.Add(_insHost);
        t.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        _contest.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 88));
        _contest.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        void Line(string label, Control c)
        {
            _contest.Controls.Add(new Label
                { Text = label, AutoSize = true, Margin = new Padding(0, 6, 8, 0) });
            _contest.Controls.Add(c);
            _contest.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        }
        Line(UiText.ContestTopLine, _contest1);
        Line(UiText.ContestBottomLine, _contest2);
        Line(UiText.ContestNumber, _contestNr);

        // Identification switch goes in a holder too, for the same
        // hide-without-shifting reason as _own.
        _idRow.Controls.Add(_showId);
        _idRow.Controls.Add(_colId);
        _subRow.Controls.Add(_showSub);
        _subRow.Controls.Add(_colSub);
        _own.Controls.Add(_idRow);
        _own.Controls.Add(_subRow);
        _own.Controls.Add(_moving);
        _own.Controls.Add(_bars);
        _own.Controls.Add(_ap2);
        _own.Controls.Add(_contest);
        _own.Controls.Add(_customPhoto);
        _own.Controls.Add(_uploadPortrait);
        // Resolution as a labelled drop-down on one row of the panel.
        // Sized by its contents, not docked: the panel it sits in is as
        // wide as its check boxes, and a docked drop-down was cut off.
        _pcmBitsRow = new TableLayoutPanel
            { ColumnCount = 2, AutoSize = true, Margin = new Padding(0), Visible = false };
        _pcmBitsRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _pcmBitsRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _pcmBitsRow.Controls.Add(new Label
            { Text = UiText.PcmResolution, AutoSize = true, Margin = new Padding(0, 6, 8, 0) }, 0, 0);
        _pcmBits.Dock = DockStyle.None;
        // 230 pushed the arrow past the panel edge at the window's
        // default size; 170 fits with the shorter item texts.
        _pcmBits.Width = 170;
        _pcmBitsRow.Controls.Add(_pcmBits, 1, 0);
        _own.Controls.Add(_pcmBitsRow);
        _own.Controls.Add(_pcmEmphasis);
        _own.Controls.Add(_pcmCtrlPic);
        _pcmTextRow = new TableLayoutPanel
            { ColumnCount = 2, AutoSize = true, Margin = new Padding(0), Visible = false };
        _pcmTextRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _pcmTextRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _pcmTextRow.Controls.Add(new Label
            { Text = UiText.PcmText, AutoSize = true, Margin = new Padding(0, 6, 8, 0) }, 0, 0);
        _pcmTextRow.Controls.Add(_pcmText, 1, 0);
        _own.Controls.Add(_pcmTextRow);
        _legacyTextRow = new TableLayoutPanel
            { ColumnCount = 2, AutoSize = true, Margin = new Padding(0), Visible = false };
        _legacyTextRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _legacyTextRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _legacyTextRow.Controls.Add(new Label
            { Text = UiText.LegacyText, AutoSize = true, Margin = new Padding(0, 6, 8, 0) }, 0, 0);
        _legacyTextRow.Controls.Add(_legacyText, 1, 0);
        _own.Controls.Add(_legacyTextRow);
        _hamQualityRow = new TableLayoutPanel
            { ColumnCount = 2, AutoSize = true, Margin = new Padding(0), Visible = false };
        _hamQualityRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _hamQualityRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _hamQualityRow.Controls.Add(new Label
            { Text = UiText.HamQuality, AutoSize = true, Margin = new Padding(0, 6, 8, 0) }, 0, 0);
        _hamQuality.Dock = DockStyle.None;
        _hamQuality.Width = 200;
        _hamQualityRow.Controls.Add(_hamQuality, 1, 0);
        _own.Controls.Add(_hamQualityRow);
        _clockTextRow = new TableLayoutPanel
            { ColumnCount = 2, AutoSize = true, Margin = new Padding(0), Visible = false };
        _clockTextRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _clockTextRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _clockTextRow.Controls.Add(new Label
            { Text = UiText.ClockText, AutoSize = true, Margin = new Padding(0, 6, 8, 0) }, 0, 0);
        _clockText.Dock = DockStyle.None;
        _clockText.Width = 170;
        _clockTextRow.Controls.Add(_clockText, 1, 0);
        _own.Controls.Add(_clockTextRow);
        _clockColorRow = new TableLayoutPanel
            { ColumnCount = 4, AutoSize = true, Margin = new Padding(0), Visible = false };
        for (int i = 0; i < 4; i++) _clockColorRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _clockColorRow.Controls.Add(new Label
            { Text = UiText.ClockColor, AutoSize = true, Margin = new Padding(0, 6, 0, 0) }, 0, 0);
        _clockColorRow.Controls.Add(_colFace, 1, 0);
        _clockColorRow.Controls.Add(new Label
            { Text = UiText.ClockBackground, AutoSize = true, Margin = new Padding(16, 6, 0, 0) }, 2, 0);
        _clockColorRow.Controls.Add(_colBack, 3, 0);
        _own.Controls.Add(_clockColorRow);
        for (int i = 0; i < _own.Controls.Count; i++)
            _own.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        t.Controls.Add(new Label { Text = "", AutoSize = true });
        t.Controls.Add(_own);
        t.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        return t;
    }

    // Anchor None in a TableLayoutPanel cell centres the control in it.
    private Control BuildPreviewHolder()
    {
        var t = new TableLayoutPanel
            { Dock = DockStyle.Fill, ColumnCount = 1, Margin = new Padding(12, 0, 0, 0) };
        t.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        t.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        // No Label above the panel: DrawPreview paints the title itself,
        // pinned just above the picture.
        t.Controls.Add(_preview);
        // A photo slot's own two buttons share the row with Save as
        // picture, which every card has: the row keeps one height, so the
        // preview above does not shrink on a photo slot.
        var buttons = new FlowLayoutPanel
            { AutoSize = true, WrapContents = false, Anchor = AnchorStyles.None, Margin = new Padding(0) };
        buttons.Controls.Add(_upload);
        buttons.Controls.Add(_empty);
        buttons.Controls.Add(_saveAs);
        t.Controls.Add(buttons);
        return t;
    }

    private void SetEnabled(bool on)
    {
        _ticker.Enabled = on;
        _aspect.Enabled = on;
        _insert.Enabled = on;
        _showId.Enabled = on;
        _showSub.Enabled = on;
        _moving.Enabled = on;
        _enabled.Enabled = on;
        _bars.Enabled = on;
        _ap2.Enabled = on;
        _contest.Enabled = on;
        // Explicit, not left to _contest's own cascade: found by
        // screenshotting the disconnected state.
        _contest1.Enabled = on;
        _contest2.Enabled = on;
        _contestNr.Enabled = on;
        _contestNr.BackColor = on ? SystemColors.Window : Theme.Paper;
        _contestNr.ForeColor = on ? SystemColors.WindowText : Theme.Muted;
        _font.Enabled = on;
        _textFace.Enabled = on;
        _y.Enabled = on;
        _speed.Enabled = on;
        _size.Enabled = on;
        _colTicker.Enabled = on;
        _colId.Enabled = on;
        _colSub.Enabled = on;
        _colIns.Enabled = on;
        _colFace.Enabled = on;
        _colBack.Enabled = on;
        _saveAs.Enabled = on;
        // Not touched anywhere else: FillFromCard() only sets their
        // Visible, and returns before that with no card selected, so
        // without this both stayed enabled and, _upload being a primary
        // button, bright blue, after a Disconnect while a Custom slot
        // was the current card.
        _upload.Enabled = on;
        _empty.Enabled = on;
        Theme.RefreshButtonEnabled(_upload, primary: true);
        _customPhoto.Enabled = on;
        _uploadPortrait.Enabled = on;
        Theme.RefreshButtonEnabled(_uploadPortrait, primary: true);
        _pcmBits.Enabled = on;
        _pcmEmphasis.Enabled = on;
        _pcmCtrlPic.Enabled = on;
        _pcmText.Enabled = on;
        _legacyText.Enabled = on;
        _hamQuality.Enabled = on;
    }
}

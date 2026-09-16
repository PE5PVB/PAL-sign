// CardsTab, the part that turns a CardInfo into what is on screen: the
// controls beside the list and the preview panel itself.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace PalSign;

public partial class CardsTab
{
    private CardInfo? Current =>
        _list.SelectedIndex >= 0 && _cfg != null && _list.SelectedIndex < _cfg.Cards.Count
            ? _cfg.Cards[_list.SelectedIndex] : null;

    private static int BandOf(CardInfo c)
    {
        if (c.TickerFont == 2 || c.TickerFont == 3)
        {
            int size = Math.Clamp(c.TickerSize, 0, 4);
            return BigBandHeight[c.TickerFont - 2][size];
        }
        return BandHeight[c.TickerFont >= 0 && c.TickerFont < BandHeight.Length ? c.TickerFont : 0];
    }

    private void SnapY()
    {
        // Not while the controls are being filled: setting Maximum below
        // the current Value makes WinForms coerce Value and raise
        // ValueChanged, which would land here with Current already
        // pointing at the new card.
        if (_loading) return;
        // Even rows only; the board rounds down to even too.
        var c = Current;
        int max = ScreenH - (c != null ? BandOf(c) : 38);
        int v = _y.Value & ~1;
        if (v > max) v = max & ~1;
        if (v != _y.Value) { _y.Value = v; return; }   // comes back here once
        if (c != null) c.TickerY = v;
        _preview.Invalidate();
    }

    private void ShowY()
    {
        var c = Current;
        int band = c != null ? BandOf(c) : 38;
        _yValue.Text = $"{_y.Value}..{_y.Value + band - 1}";
    }

    private void ShowSpeed()
    {
        int v = _speed.Value;
        _speedValue.Text = v < 0 ? $"{-v} left" : $"{v} right";
    }

    private void FillFromCard()
    {
        var c = Current;
        if (c == null || _cfg == null) return;
        if (_stale.Contains(c.Number) && !_busy) { _wanted = c.Number; Run(FetchOne); }
        _loading = true;
        try
        {
            // Transparent is not offered on an uploaded photograph: such
            // a card reads its rows from flash, with no room for a band.
            _ticker.Items.Clear();
            _ticker.Items.Add(UiText.TickerOff);
            _ticker.Items.Add(UiText.TickerSolid);
            if (!c.Photo) _ticker.Items.Add(UiText.TickerTransparent);
            _ticker.SelectedIndex = Math.Min(c.Ticker, _ticker.Items.Count - 1);

            _font.SelectedIndex = Math.Min(c.TickerFont, _font.Items.Count - 1);
            _aspect.SelectedIndex = Math.Min(c.Aspect, Math.Max(0, _aspect.Items.Count - 1));
            _insert.SelectedIndex = Math.Min(c.Insert, Math.Max(0, _insert.Items.Count - 1));
            // Greyed on an empty slot: the board refuses the set there
            // regardless of the stored flag.
            bool emptySlot = c.Photo && c.Name == "Empty slot";
            _enabled.Checked = c.Enabled && !emptySlot;
            _enabled.Enabled = !emptySlot;
            _textFace.SelectedIndex = Math.Clamp(c.TextFont, 0, _textFace.Items.Count - 1);
            // Mixed bars always letters its line in Inter (FixedFont), so
            // the choice is hidden there even though it draws text.
            bool anyText = !c.FixedFont && (c.HasId != 1 || c.HasSub != 1 || c.HasBox != 1);
            _textFaceLabel.Visible = anyText;
            _textFace.Visible = anyText;
            _colTicker.BackColor = CardPreview.ColorOf(c.TickerColor);
            _colId.BackColor = CardPreview.ColorOf(c.IdColor);
            _colSub.BackColor = CardPreview.ColorOf(c.SubColor);
            _colIns.BackColor = CardPreview.ColorOf(c.InsColor);
            _showId.Checked = c.ShowId;
            // The board says which cards have a place for each of these;
            // not known yet counts as shown.
            _idRow.Visible = c.HasId != 1;
            _showSub.Checked = c.ShowSub;
            _subRow.Visible = c.HasSub != 1;
            _moving.Checked = c.Moving;
            _moving.Visible = c.HasLine != 1;
            bool boxes = c.HasBox != 1;
            _insertLabel.Visible = boxes;
            _insHost.Visible = boxes;
            _bars.Visible = c.Own == 1;
            _ap2.Visible = c.Own == 2;
            _contest.Visible = c.Own == 3;
            _customPhoto.Visible = c.Own == 4;
            _uploadPortrait.Visible = c.Own == 4;
            bool sony = c.Own == 5, ham = c.Own == 6;   // no ticker, no aspect ratio on either
            _pcmBitsRow.Visible = sony;
            _pcmEmphasis.Visible = sony;
            _pcmCtrlPic.Visible = sony;
            _pcmTextRow.Visible = ham;
            _hamQualityRow.Visible = ham;
            ShowRasterRows(!(sony || ham));
            _bars.Checked = _cfg.G924Bars;
            _ap2.Checked = _cfg.TcgAp2;
            _customPhoto.Checked = c.CustomPhoto;
            _pcmBits.SelectedIndex = (c.Option & 1) != 0 ? 1 : 0;
            _pcmEmphasis.Checked = (c.Option & 2) != 0;
            _pcmCtrlPic.Checked = (c.Option & 4) != 0;
            _contest1.Text = _cfg.Contest1;
            _contest2.Text = _cfg.Contest2;
            _contestNr.Value = Math.Clamp(_cfg.ContestNr, 0, 9999);
            // What came off the board counts as sent; see SendOnLeave().
            _contest1.Tag = _contest1.Text;
            _contest2.Tag = _contest2.Text;
            _hamQuality.SelectedIndex = c.Option & 3;
            _pcmText.Text = _cfg.PcmText;
            _pcmText.Tag = _pcmText.Text;

            _y.Maximum = ScreenH - BandOf(c);
            _y.Value = Math.Min(c.TickerY, _y.Maximum) & ~1;
            int sp = Math.Clamp(c.TickerSpeed, _speed.Minimum, _speed.Maximum);
            _speed.Value = sp == 0 ? -1 : sp;   // zero cannot be stored
            _speedPrev = _speed.Value;
            ShowSpeed();
            ShowY();
            _size.SelectedIndex = Math.Clamp(c.TickerSize, 0, 4);

            bool on = c.Ticker != 0;   // greyed, not hidden, so rows do not reflow
            _y.Enabled = on;
            _font.Enabled = on;
            _speed.Enabled = on;
            // Only Inter (2) and Doto (3) come in the five fixed
            // sizes; see TICKER_FACE_BIG_INTER/SQUARE in ticker.cpp.
            _size.Enabled = on && c.TickerFont >= 2;

            _upload.Visible = c.Photo;
            _empty.Visible = c.Photo;
            _empty.Enabled = c.Photo && !emptySlot;
        }
        finally { _loading = false; }
        _preview.Invalidate();
    }

    // Read from the aspect NAME the board sends, rather than a local
    // copy of aspect.cpp's table, so the two cannot drift apart.
    private (int lines, bool atTop) LetterboxOf(CardInfo c)
    {
        if (_cfg == null || c.Aspect < 0 || c.Aspect >= _cfg.AspectNames.Count) return (576, false);
        string n = _cfg.AspectNames[c.Aspect].ToLowerInvariant();
        bool top = n.Contains("at the top");
        if (!n.Contains("letterbox")) return (576, false);
        if (n.Contains("14:9")) return (504, top);
        if (n.Contains("wider")) return (326, top);
        if (n.Contains("16:9")) return (430, top);
        return (576, false);
    }

    private void DrawPreview(object? sender, PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.Clear(SystemColors.Control);
        var c = Current;
        if (c == null)
        {
            using var pale = new SolidBrush(Color.FromArgb(110, 110, 110));
            g.DrawString(_cfg == null ? UiText.PressConnectFirst
                                      : UiText.NoCardChosen,
                         Font, pale, 6, 6);
            return;
        }

        var box = _preview.ClientRectangle;
        box.Inflate(-4, -4);
        if (box.Width < 40 || box.Height < 30) return;
        // Pinned to the bottom edge at a fixed gap to the save button,
        // whatever the window height; a strip above is held back for
        // the title.
        const int titleRoom = 18;
        float fit = Math.Min(box.Width / 720f, (box.Height - titleRoom) / 576f);
        int need = (int)(576 * fit);
        int drawn = (int)(720 * fit);
        box.Y = box.Bottom - need;
        box.Height = need;
        using (var title = new SolidBrush(Color.FromArgb(60, 60, 60)))
        {
            SizeF sz = g.MeasureString(UiText.PreviewTitle, Font);
            g.DrawString(UiText.PreviewTitle, Font, title,
                         box.X + (drawn - sz.Width) / 2, box.Y - sz.Height - 3);
        }

        _picture.TryGetValue(c.Number, out Image? shot);
        // Letterbox and squeeze applied here, not on the board's own
        // picture, so changing the WSS costs no fetch.
        var (lines, atTop) = LetterboxOf(c);
        var r = CardPreview.Draw(g, box, c.Name, shot, lines, atTop);
        var band = CardPreview.ActiveBand(r, lines, atTop);
        // The clock is not part of the fetched picture; drawn here, live.
        if (shot != null && _boxes.TryGetValue(c.Number, out CardBoxes? bx))
            CardPreview.Clock(g, band, bx, _boardClock + (DateTime.UtcNow - _readAt), _dateFormat,
                              _faces.TryGetValue(c.TextFont, out BoardFont? bf) ? bf : null,
                              CardPreview.ColorOf(c.InsColor));
        CardPreview.Letterbox(g, r, lines, atTop);
        // The ticker sits outside the aspect ratio, on the board too.
        CardPreview.Ticker(g, r, c.Ticker, c.TickerY, BandOf(c), _cfg?.TickerText ?? "", _scroll,
                           CardPreview.ColorOf(c.TickerColor));
        // The moving stripe is not in the fetched picture either.
        if (c.Moving && shot != null && _boxes.TryGetValue(c.Number, out CardBoxes? mb))
            CardPreview.MovingLine(g, band, mb, DateTime.UtcNow.TimeOfDay.TotalSeconds);
        g.DrawRectangle(Pens.Black, r.X, r.Y, r.Width - 1, r.Height - 1);
    }

    private void DrawCard(object? sender, DrawItemEventArgs e)
    {
        if (e.Index < 0 || _cfg == null || e.Index >= _cfg.Cards.Count) return;
        var c = _cfg.Cards[e.Index];
        e.DrawBackground();
        bool sel = (e.State & DrawItemState.Selected) != 0;
        var ink = sel ? SystemColors.HighlightText : SystemColors.WindowText;

        // The card on the television gets an arrow and bold text.
        bool active = c.Number == _cfg.CurrentCard;
        string mark = c.Ticker == 1 ? "■" : c.Ticker == 2 ? "□" : " ";
        // Grey for an empty slot or a disabled card, same as being
        // skipped when stepping through.
        bool empty = c.Photo && c.Name == "Empty slot";
        bool grey = (empty || !c.Enabled) && !sel;
        using var brush = new SolidBrush(grey ? Theme.Muted : active && !sel ? Theme.Accent : ink);
        using var bold = active ? new Font(e.Font ?? Font, FontStyle.Bold) : null;
        e.Graphics.DrawString($"{(active ? "►" : " ")}{c.Number,2} {mark} {c.Name}",
                              bold ?? e.Font ?? Font, brush,
                              e.Bounds.X + 2, e.Bounds.Y + 2);
        e.DrawFocusRectangle();
    }
}

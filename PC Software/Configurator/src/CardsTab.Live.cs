// CardsTab, the part that pushes a change to the board while it is being
// made, and walks the preview's own band in step with it.
using System;
using System.Drawing;
using System.Threading;
using System.Windows.Forms;

namespace PalSign;

public partial class CardsTab
{
    private void Push(string field, int value)
    {
        if (_loading) return;
        var c = Current;
        if (c == null) return;

        switch (field)
        {
            case "ticker": c.Ticker = value; break;
            case "tickery": c.TickerY = value; break;
            case "tickerfont": c.TickerFont = value; break;
            case "tickerspeed": c.TickerSpeed = value; break;
            case "tickerscale": c.TickerSize = value; break;
            case "textfont": c.TextFont = value; break;
            case "aspect": c.Aspect = value; break;
            case "insert": c.Insert = value; break;
            case "showid": c.ShowId = value != 0; break;
            case "showsub": c.ShowSub = value != 0; break;
            case "moving": c.Moving = value != 0; break;
            case "enabled": c.Enabled = value != 0; break;
            case "customphoto": c.CustomPhoto = value != 0; break;
            case "option": c.Option = value; break;
        }
        if (field == "ticker" || field == "tickerfont") FillFromCard();
        _list.Invalidate();
        _preview.Invalidate();

        // By name, so a value set again before it went over replaces the
        // one waiting instead of queueing behind it.
        _pending[$"card.{c.Number}.{field}"] = value.ToString();
    }

    private void WireSwatch(Button b, string field, string tip)
    {
        _why.SetToolTip(b, tip);
        b.Click += (_, __) =>
        {
            if (Current == null) return;
            using var dlg = new ColorDialog { FullOpen = true, Color = b.BackColor };
            if (dlg.ShowDialog(FindForm()) != DialogResult.OK) return;
            b.BackColor = dlg.Color;
            PushColor(field, dlg.Color);
        };
    }

    private void PushColor(string field, Color col)
    {
        if (_loading) return;
        var c = Current;
        if (c == null) return;
        string hex = $"{col.R:X2}{col.G:X2}{col.B:X2}";
        switch (field)
        {
            case "tickercolor": c.TickerColor = hex; break;
            case "idcolor": c.IdColor = hex; break;
            case "subcolor": c.SubColor = hex; break;
            case "inscolor": c.InsColor = hex; break;
        }
        _preview.Invalidate();
        _pending[$"card.{c.Number}.{field}"] = hex;
    }

    // Not scoped to a card, but shown beside one; same queue, same open
    // port. The copy in _cfg is kept in step for when the card is shown
    // again.
    private void PushShared(string name, int value)
    {
        if (_loading || _cfg == null) return;
        if (name == "g924bars") _cfg.G924Bars = value != 0;
        if (name == "tcgap2") _cfg.TcgAp2 = value != 0;
        if (name == "contestnr") _cfg.ContestNr = value;
        _pending[name] = value.ToString();
    }

    private void PushShared(string name, string value)
    {
        if (_loading || _cfg == null) return;
        if (name == "contest1") _cfg.Contest1 = value;
        if (name == "contest2") _cfg.Contest2 = value;
        if (name == "pcmtext") _cfg.PcmText = value;
        _pending[name] = value;
    }

    // Only when it really changed: sending an unchanged text would still
    // make the board rebuild the card. Last sent value kept in Tag.
    private void SendOnLeave(TextBox box, string name)
    {
        void Send()
        {
            if (_loading) return;
            if (box.Tag as string == box.Text) return;
            box.Tag = box.Text;
            PushShared(name, box.Text);
        }
        box.Leave += (_, __) => Send();
        box.KeyDown += (_, e) =>
        {
            if (e.KeyCode != Keys.Enter) return;
            e.SuppressKeyPress = true;
            Send();
        };
    }

    // One setting a tick: this runs on the drawing thread, and emptying
    // the whole queue at once would be felt as the window stopping.
    private void Drain()
    {
        if (_busy) return;
        if (_pending.Count == 0)
        {
            _quiet++;
            // Half a second of quiet first, so dragging a slider does
            // not ask this after every step.
            if (_askCrc && _quiet > 12) { AskCardCrc(); return; }
            if (_refreshDue) { _refreshDue = false; DropLive(); Run(ReadCrcsAgain); return; }
            // About one and a half seconds a poll: often enough to feel
            // live next to a slideshow step (SLIDE_HOLD_MS and every
            // sequence entry are seconds, not fractions of one), rare
            // enough not to fight the port with whatever else wants it.
            if (_slideshowRunning)
            {
                _slidePollTicks++;
                if (_slidePollTicks > 37) { _slidePollTicks = 0; AskCurrentCard(); return; }
            }
            // Dropped after a second of quiet regardless of
            // _slideshowRunning: SerialPort only ever allows one open
            // handle on a COM port, and holding this one open between
            // polls shut Stop -- and the Slideshow tab's own Read/Write,
            // and everything else -- out of the port entirely, since
            // each of those opens its own separate connection rather
            // than sharing this one.
            if (_live != null && _quiet > 25) DropLive();
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
            string reply = BoardConfig.SetLive(_live, name, value);
            Session.Dirty = true;

            // A card-scoped set answers with that card's new checksum,
            // so nothing further is asked. A shared setting can touch
            // any card, and reading the whole dump to find out costs a
            // black flash; that happens at Connect and after a save
            // instead.
            uint? crc = BoardConfig.CrcIn(reply);
            if (crc != null && name.StartsWith("card.", StringComparison.Ordinal))
                TakeCrc(name, crc.Value);
            else
            {
                _sharedChanged = true;
                _askCrc = true;
            }
        }
        catch (Exception ex)
        {
            // Only the change that failed is dropped; it is already out
            // of the queue.
            DropLive();
            Say(ex.Message);
        }
    }

    private void TakeCrc(string name, uint crc)
    {
        string[] part = name.Split('.');
        if (part.Length < 2 || !int.TryParse(part[1], out int number)) return;
        TakeCrc(number, crc);
    }

    private void TakeCrc(int number, uint crc)
    {
        if (_cfg == null) return;
        foreach (var c in _cfg.Cards)
        {
            if (c.Number != number) continue;
            if (c.Crc == crc) return;
            c.Crc = crc;
            _stale.Add(number);
            if (!_busy) { _wanted = number; Run(FetchOne); }
            return;
        }
    }

    private void Roll()
    {
        var c = Current;
        if (c == null || _busy) return;
        bool boxes = _boxes.TryGetValue(c.Number, out CardBoxes? bx);
        bool clock = boxes && bx!.HasClock;
        bool line = boxes && bx!.HasLine && c.Moving;
        if (c.Ticker == 0 && !clock && !line) return;
        // Same speed ladder as tickerFrame() on the board.
        int v = Math.Clamp(Current?.TickerSpeed ?? -4, -8, 8);
        int[] div = { 1, 4, 3, 2, 1, 1, 1, 1, 1 };
        int[] step = { 2, 2, 2, 2, 2, 4, 6, 8, 10 };
        int lvl = Math.Abs(v);
        if (v != 0 && (div[lvl] <= 1 || ++_scrollPhase >= div[lvl]))
        {
            _scrollPhase = 0;
            int d = v < 0 ? step[lvl] : -step[lvl];
            _scroll = ((_scroll + d) % CardPreview.ScreenW + CardPreview.ScreenW)
                      % CardPreview.ScreenW;
        }
        _preview.Invalidate();
    }

    /// <summary>Lets go of the live port so something else can open one.</summary>
    public void ReleasePort() => DropLive();

    private void DropLive()
    {
        if (_live == null) return;
        BoardConfig.CloseLive(_live);
        _live = null;
        _quiet = 0;
    }
}

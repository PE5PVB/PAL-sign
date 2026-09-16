// CardsTab, the part that talks to the board on its own thread: Connect,
// Adopt, the picture fetches and the progress dialog that covers them.
using System;
using System.Collections.Generic;
using System.Threading;

namespace PalSign;

public partial class CardsTab
{
    // On a thread of its own: the board gives up sending a card if this
    // side stops reading for a second, and a message pump in the loop
    // (needed to keep a progress bar moving) is enough to trigger that.
    // Every touch of the window goes back through Post().
    /// <summary>Everything with the board in one go, for the Connect
    /// button up in the bar.</summary>
    public void ConnectNow() => Run(Connect);

    /// <summary>Throws the kept pictures away and fetches everything
    /// again, for the button at the bottom of the System tab.
    /// The clearing happens inside Run(), so a press while something
    /// else is busy does nothing rather than wiping the previews without
    /// fetching them back.</summary>
    public void FetchAllAgain()
    {
        Run(() =>
        {
            Post(() =>
            {
                foreach (var img in _picture.Values) img.Dispose();
                _picture.Clear();
                _boxes.Clear();
            });
            CardCache.Forget();
            Connect();
        });
    }

    private void Connect()
    {
        string? p = _port();
        if (p == null) { Post(() => Say(UiText.NoPortChosen)); return; }

        // The clock is set first, so a card fetched below carries this
        // computer's time; the board has no battery and restarts at the
        // time in config.h.
        try { BoardConfig.SetClock(p, DateTime.Now, CancellationToken.None); }
        catch (Exception ex) { Trace.Say("the clock was not set: " + ex.Message); }

        Trace.Say("--- Connect ---");
        var cfg = BoardConfig.Read(p, CancellationToken.None);
        Trace.Say($"read: proto {cfg.Proto}, {cfg.Cards.Count} cards, on card {cfg.CurrentCard}");
        // One face per distinct font among the cards with insert boxes;
        // missing one is not fatal, the clock just falls back to a
        // Windows font.
        var faces = new Dictionary<int, BoardFont>();
        foreach (var c in cfg.Cards)
        {
            if (c.HasBox != 2 || faces.ContainsKey(c.TextFont)) continue;
            try { faces[c.TextFont] = BoardFont.Fetch(p, c.TextFont, CancellationToken.None); }
            catch (Exception ex)
            {
                Trace.Say($"face {c.TextFont} did not come over: " + ex.Message);
            }
        }

        Post(() => Adopt(cfg, faces));
        FetchPictures(p, cfg);
        Session.Dirty = cfg.Dirty;
        Post(() => Say(""));
    }

    // Everything that touches the window, in one place, run on its thread.
    private void Adopt(BoardSettings cfg, Dictionary<int, BoardFont> faces)
    {
        // The only place the pending queue is thrown away, since the
        // model it referred to is being replaced.
        _pending.Clear();
        _refreshDue = false;
        _sharedChanged = false;
        _askCrc = false;
        _cfg = cfg;
        _faces = faces;
        _boardClock = cfg.Clock;
        _readAt = DateTime.UtcNow;
        _dateFormat = cfg.DateFormat;

        _loading = true;
        try
        {
            _aspect.Items.Clear();
            foreach (var n in cfg.AspectNames) _aspect.Items.Add(n);
            _insert.Items.Clear();
            // The board does not name these itself; there are four and
            // they are what the i key walks.
            _insert.Items.Add(UiText.InsertNone);
            _insert.Items.Add(UiText.InsertTime);
            _insert.Items.Add(UiText.InsertDateTime);
            _insert.Items.Add(UiText.InsertFixedTexts);
            _font.Items.Clear();
            _font.Items.Add(UiText.FontPm5544);
            _font.Items.Add(UiText.FontPm8546);
            _font.Items.Add(UiText.FontInter);
            _font.Items.Add(UiText.FontSquare);   // index 3, same order as FONTS[]

            _list.Items.Clear();
            foreach (var c in cfg.Cards) _list.Items.Add(c);
        }
        finally { _loading = false; }

        SetEnabled(true);
        if (_list.Items.Count > 0)
        {
            // By card number, not by list position: a card the board
            // hides (a PCM card with no ADC attached) leaves a gap in
            // the numbering.
            int sel = 0;
            for (int i = 0; i < cfg.Cards.Count; i++)
            {
                if (cfg.Cards[i].Number == cfg.CurrentCard) { sel = i; break; }
            }
            _list.SelectedIndex = sel;
        }
        Settings?.Invoke(cfg);
        Live?.Invoke(true);
    }

    /// <summary>Lets the board go: closes the live port and forgets
    /// everything read off the board. The pictures on disk stay, kept
    /// under their checksum, so the next Connect reuses them.</summary>
    public void Disconnect()
    {
        DropLive();
        _pending.Clear();
        _refreshDue = false;
        _stale.Clear();
        _cfg = null;
        _list.Items.Clear();
        SetEnabled(false);
        _preview.Invalidate();
        Say(UiText.LetGoOfBoard);
        Live?.Invoke(false);
    }

    // Only what is out of date crosses the port: the board's CRC per
    // card decides, and a kept picture under that CRC is still right.
    // The first Connect takes a while because every card must be built
    // on the board to be captured; later ones almost skip this.
    private void FetchPictures(string portName, BoardSettings cfg)
    {
        var todo = new List<CardInfo>();
        foreach (var c in cfg.Cards)
        {
            // A picture just uploaded from this side: the board holds
            // exactly these bytes, so it is decoded and kept without a
            // fetch.
            if (_fresh.TryGetValue(c.Number, out byte[]? raw))
            {
                _fresh.Remove(c.Number);
                var fresh = CardCache.Decode(raw);
                CardCache.Save(c.Number, c.Crc, fresh, CardBoxes.None);
                Post(() => { Keep(c.Number, fresh); _boxes[c.Number] = CardBoxes.None; });
                continue;
            }
            var (have, crc, boxes) = CardCache.LoadAny(c.Number);
            // A copy on disk may predate the line-box field in the
            // sidecar format and lack it even at the right CRC.
            if (have != null && crc == c.Crc && c.HasLine == 2 && !boxes.HasLine)
            {
                have.Dispose();
                todo.Add(c);
                continue;
            }
            if (have != null && crc == c.Crc)
            {
                var picture = have;
                Post(() => { Keep(c.Number, picture); _boxes[c.Number] = boxes; });
            }
            else { have?.Dispose(); todo.Add(c); }
        }
        Trace.Say($"{cfg.Cards.Count - todo.Count} pictures were already here, {todo.Count} to fetch");
        if (todo.Count == 0) { Post(() => _preview.Invalidate()); return; }

        ProgressOpen(string.Format(UiText.FetchingCountFmt, todo.Count));
        System.IO.Ports.SerialPort? port = null;
        try
        {
            port = CardCache.OpenFetch(portName);
            for (int i = 0; i < todo.Count; i++)
            {
                var c = todo[i];
                int done = i + 1, total = todo.Count;
                ProgressText(string.Format(UiText.FetchingCardOfFmt, c.Name, done, total));
                try
                {
                    var (shot, boxes) = CardCache.Fetch(port, c.Number, Step,
                                                       CancellationToken.None);
                    CardCache.Save(c.Number, c.Crc, shot, boxes);
                    Trace.Say($"card {c.Number} kept on disk");
                    Post(() =>
                    {
                        Keep(c.Number, shot);
                        _boxes[c.Number] = boxes;
                        _preview.Invalidate();
                    });
                }
                catch (Exception ex)
                {
                    Post(() => Say($"{c.Name}: {ex.Message}"));
                    return;
                }
            }
        }
        finally
        {
            if (port != null) CardCache.CloseFetch(port);
            ProgressClose();
            Post(() => _preview.Invalidate());
        }
    }

    /// <summary>Says a shared setting changed elsewhere, so a kept
    /// picture may be out of date. Acted on at the next save.</summary>
    public void NoteSharedChange()
    {
        _sharedChanged = true;
        _askCrc = true;
    }

    // The one card being looked at, in sixty bytes instead of the whole
    // dump: a shared setting can change what it looks like, and this
    // avoids the black flash a full dump costs.
    private void AskCardCrc()
    {
        var c = Current;
        string? p = _port();
        if (c == null || p == null) { _askCrc = false; return; }
        try
        {
            _live ??= BoardConfig.OpenLive(p);
            string reply = BoardConfig.SetLive(_live, "cardcrc",
                                               c.Number.ToString());
            _askCrc = false;
            uint? crc = BoardConfig.CrcIn(reply);
            if (crc != null) TakeCrc(c.Number, crc.Value);
        }
        catch (Exception)
        {
            DropLive();   // likely the System tab still has the port; comes round again
        }
    }

    /// <summary>Whether a slideshow is stepping through cards on its
    /// own; see SlideshowTab.cs. While it is, the list and the preview
    /// otherwise keep showing whichever card was current at Connect or
    /// the last manual switch, since nothing else here ever re-asks.
    /// </summary>
    public void SetSlideshowRunning(bool on)
    {
        _slideshowRunning = on;
        _slidePollTicks = 0;
    }

    // Which card the board is actually showing, the same "riding the
    // set command" shape as AskCardCrc()'s own cardcrc; see
    // "currentcard" in Firmware.ino's setSettingFromCommand(). Polled
    // rather than pushed: the board has no way to speak up on its own
    // between requests, only answer one.
    private void AskCurrentCard()
    {
        string? p = _port();
        if (p == null || _cfg == null) return;
        try
        {
            _live ??= BoardConfig.OpenLive(p);
            string reply = BoardConfig.SetLive(_live, "currentcard", "0");
            int? card = BoardConfig.CurrentCardIn(reply);
            if (card != null && card != _cfg.CurrentCard)
            {
                _cfg.CurrentCard = card.Value;
                _list.Invalidate();
                _preview.Invalidate();
            }
        }
        catch (Exception)
        {
            DropLive();   // likely the System tab still has the port; comes round again
        }
    }

    /// <summary>Brings the pictures up to date after a save, one of the
    /// two moments the whole dump is read (the other being Connect).</summary>
    public void RefreshAfterSave()
    {
        if (!_sharedChanged) return;
        _sharedChanged = false;
        AskRefresh();
    }

    private void AskRefresh()
    {
        _refreshDue = true;
        _wanted = Current?.Number ?? 0;   // captured now, the selection can move before the read runs
    }

    private void RefreshIfDue()
    {
        // Not while there is still something to send, or a dragged
        // slider would trigger a dump per step.
        if (!_refreshDue || _busy || _cfg == null || _pending.Count > 0) return;
        _refreshDue = false;
        Run(ReadCrcsAgain);
    }

    private void ReadCrcsAgain()
    {
        string? p = _port();
        if (p == null || _cfg == null) return;
        Trace.Say("--- reading the checksums again after a change ---");
        var fresh = BoardConfig.Read(p, CancellationToken.None);

        var missed = new List<int>();
        var same = new List<int>();
        foreach (var c in fresh.Cards)
        {
            CardInfo? old = null;
            foreach (var o in _cfg.Cards) if (o.Number == c.Number) { old = o; break; }
            if (old == null) continue;
            if (old.Crc == c.Crc) { same.Add(c.Number); continue; }
            old.Crc = c.Crc;
            missed.Add(c.Number);
        }
        Post(() => { foreach (int n in same) _stale.Remove(n); });
        if (missed.Count == 0) { Trace.Say("nothing changed about any picture"); return; }
        Trace.Say($"{missed.Count} of the pictures are out of date");
        Post(() =>
        {
            foreach (int n in missed) _stale.Add(n);
            // The cards not on screen are not fetched now; said out loud
            // so a busy Connect afterwards is not a surprise.
            if (missed.Count > 1)
                Say($"{missed.Count} pictures are now out of date; they are fetched "
                    + "when you select them, or all at once at the next Connect");
        });

        if (missed.Contains(_wanted)) FetchInto(p, _wanted);
    }

    private void FetchOne()
    {
        string? p = _port();
        if (p != null) FetchInto(p, _wanted);
    }

    private void FetchInto(string portName, int number)
    {
        CardInfo? c = null;
        if (_cfg != null) foreach (var o in _cfg.Cards) if (o.Number == number) { c = o; break; }
        if (c == null) return;

        ProgressOpen(string.Format(UiText.FetchingOneFmt, c.Name));
        System.IO.Ports.SerialPort? port = null;
        try
        {
            port = CardCache.OpenFetch(portName);
            var (shot, boxes) = CardCache.Fetch(port, number, Step, CancellationToken.None);
            CardCache.Save(number, c.Crc, shot, boxes);
            Post(() =>
            {
                Keep(number, shot);
                _boxes[number] = boxes;
                _stale.Remove(number);
                _preview.Invalidate();
                Say("");
            });
        }
        catch (Exception ex) { Post(() => Say(ex.Message)); }
        finally
        {
            if (port != null) CardCache.CloseFetch(port);
            ProgressClose();
        }
    }

    // A modal window, not a bar in the button row, so nothing can be
    // clicked halfway through a fetch. Everything goes through Post, so
    // the worker never waits on the window.
    private ProgressDialog? _progress;

    private void ProgressOpen(string text, bool fake = false)
    {
        Post(() =>
        {
            if (_progress != null) { _progress.Report(text, 0); return; }
            using var d = new ProgressDialog(UiText.TalkingTitle);
            _progress = d;
            d.Report(text, 0);
            if (fake) d.FakePace();
            d.ShowDialog(FindForm());
            _progress = null;
        });
    }

    private void ProgressText(string text) => Post(() => _progress?.Report(text, -1));
    private void ProgressClose() => Post(() => _progress?.Close());

    private void Step(int percent) => Post(() => _progress?.Report(null, percent));

    // Never silently dropped: a tab page has no window handle until
    // shown once, so if there is none yet and this is already the
    // drawing thread, run it directly instead of losing the update.
    private void Post(Action what)
    {
        if (IsDisposed) return;
        if (!IsHandleCreated)
        {
            if (!InvokeRequired) what();
            return;
        }
        try { BeginInvoke(what); }
        catch (Exception) { /* the window is going */ }
    }
}

// CardsTab, the part that deals with the sixteen uploadable slots.
using System;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Windows.Forms;

namespace PalSign;

public partial class CardsTab
{
    // What came off the board, with the clock baked in for the cards
    // that have one: the fetched picture never carries it (see
    // CardCache.cs), so a plain save would show empty insert boxes on
    // those three cards. The ticker and the moving line stay out, as
    // before: they animate, and there is no "current" position of
    // theirs that belongs in a saved picture the way a clock reading
    // does.
    private void SavePicture()
    {
        var c = Current;
        if (c == null || !_picture.TryGetValue(c.Number, out Image? shot))
        {
            Say(UiText.NoPictureYet);
            return;
        }
        string safe = c.Name;
        foreach (char bad in Path.GetInvalidFileNameChars()) safe = safe.Replace(bad, '_');
        using var box = new SaveFileDialog
        {
            Title = UiText.SaveCardTitle,
            Filter = UiText.PngFilter,
            FileName = $"card-{c.Number:D2}-{safe}.png",
            DefaultExt = "png",
        };
        if (box.ShowDialog(FindForm()) != DialogResult.OK) return;
        try
        {
            using var composed = new Bitmap(shot.Width, shot.Height);
            using (var g = Graphics.FromImage(composed))
            {
                g.DrawImage(shot, 0, 0, shot.Width, shot.Height);
                if (_boxes.TryGetValue(c.Number, out CardBoxes? bx))
                    CardPreview.Clock(g, new Rectangle(0, 0, shot.Width, shot.Height), bx,
                                      _boardClock + (DateTime.UtcNow - _readAt), _dateFormat,
                                      _faces.TryGetValue(c.TextFont, out BoardFont? bf) ? bf : null,
                                      CardPreview.ColorOf(c.InsColor));
            }
            composed.Save(box.FileName, System.Drawing.Imaging.ImageFormat.Png);
            Say(string.Format(UiText.WrittenToFmt, Path.GetFileName(box.FileName)));
        }
        catch (Exception ex) { Say(ex.Message); }
    }

    // The picture is not fetched back afterwards: the board holds
    // exactly the uploaded bytes, kept here for FetchPictures to use.
    private void UploadPicture()
    {
        var c = Current;
        if (c == null || !c.Photo || _cfg == null) return;
        int slot = SlotOf(c);
        if (slot < 1) return;
        // Same "Empty slot" test DrawCard() already uses, in
        // CardsTab.Display.cs.
        bool alreadyFilled = c.Name != "Empty slot";
        using (var dialog = new PhotoDialog(_port, slot, $"Custom {slot}", alreadyFilled))
        {
            // Only on a real upload: ConnectNow() re-reads the whole
            // board, which would be pure waste after a dialog that was
            // simply cancelled.
            if (dialog.ShowDialog(FindForm()) != DialogResult.OK || dialog.UploadedBytes == null) return;
            _fresh[c.Number] = dialog.UploadedBytes;
        }
        ConnectNow();
    }

    // BBC Test Card F and W share the one uploaded photo, so this does
    // not need a card selected the way UploadPicture() does. A
    // successful upload can only be seen with the switch on and a
    // fetch redone, since the picture PhotoDialog fetches back is what
    // the board's own renderRow() drew at the time, before the upload;
    // ConnectNow() re-reads everything rather than tracking just these
    // two cards as stale.
    private void UploadPortrait()
    {
        using var dialog = new PortraitDialog(_port);
        if (dialog.ShowDialog(FindForm()) != DialogResult.OK) return;
        ConnectNow();
    }

    private void EmptySlot()
    {
        var c = Current;
        string? p = _port();
        if (c == null || !c.Photo || p == null) return;
        int slot = SlotOf(c);
        if (slot < 1) return;
        if (MessageBox.Show(FindForm(), string.Format(UiText.EmptySlotAskFmt, slot, c.Name),
                            UiText.EmptySlotTitle, MessageBoxButtons.OKCancel,
                            MessageBoxIcon.Warning) != DialogResult.OK) return;
        Run(() =>
        {
            // Fake pace: the board says nothing for seconds between the
            // first "[erase]" line and "done". See ProgressDialog.FakePace.
            ProgressOpen(string.Format(UiText.ErasingFmt, c.Name), fake: true);
            try { BoardSlots.Erase(p, slot, t => Trace.Say(t), CancellationToken.None); }
            finally { ProgressClose(); }
            Post(() => Say(string.Format(UiText.SlotEmptiedFmt, c.Name)));
            Connect();
        });
    }

    // Counted rather than assumed, so an extra card in front of the
    // uploadable ones cannot shift the numbering silently.
    private int SlotOf(CardInfo card)
    {
        if (_cfg == null) return -1;
        int n = 0;
        foreach (var c in _cfg.Cards)
        {
            if (!c.Photo) continue;
            n++;
            if (c.Number == card.Number) return n;
        }
        return -1;
    }

    private void Keep(int number, Image picture)
    {
        if (_picture.TryGetValue(number, out Image? old) && !ReferenceEquals(old, picture))
            old.Dispose();
        _picture[number] = picture;
    }

    private void ShowOnBoard(int number)
    {
        string? p = _port();
        if (p == null) return;
        BoardConfig.Set(p, "card", number.ToString(), CancellationToken.None);
        Post(() =>
        {
            Say(string.Format(UiText.CardOnScreenFmt, number));
            if (_cfg != null) _cfg.CurrentCard = number;
            _list.Invalidate();
        });
    }
}

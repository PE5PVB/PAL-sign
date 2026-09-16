// The photograph page as a window of its own, opened with a known slot
// (the card it came from), wrapping a PhotoPanel.
using System;
using System.Drawing;
using System.Windows.Forms;

namespace PalSign;

public class PhotoDialog : Form
{
    public PhotoDialog(Func<string?> port, int slot, string cardName, bool alreadyFilled)
    {
        Text = string.Format(UiText.PictureForFmt, cardName);
        Size = new Size(760, 840);
        MinimumSize = new Size(620, 620);
        StartPosition = FormStartPosition.CenterParent;
        MinimizeBox = false;

        var page = new PhotoPanel(port, cardName, alreadyFilled) { Dock = DockStyle.Fill };
        page.PickSlot(slot);
        // Bytes travel along so CardsTab can keep them without re-fetching.
        page.Uploaded += (bytes, _) =>
        {
            UploadedBytes = bytes;
            DialogResult = DialogResult.OK;
            Close();
        };
        Controls.Add(page);
    }

    /// <summary>The 4:2:2 bytes of a successful upload, or null when
    /// nothing went over.</summary>
    public byte[]? UploadedBytes { get; private set; }
}

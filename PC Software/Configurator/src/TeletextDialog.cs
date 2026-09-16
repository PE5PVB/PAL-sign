// The board's own teletext page, one row per line (1..TT_ROWS, 40
// columns; see Firmware/src/teletext.h). Styled like the page itself,
// white text on black, borderless and edge to edge, so the layout is
// what actually goes to air.
//
// A fixed ClientSize, not Form.AutoSize: a Form auto-sizing around
// several stacked Dock.Top children does not reliably pick up their
// width, and came out a sliver wide when tried.
//
// COLOUR. ETS 300 706 12.2's alpha colour codes (0x01..0x07) are
// "Set-After": one code occupies a character cell of its own (shown as
// a space) and colours everything after it up to the next code or the
// end of the row. No firmware change was needed for this: teletext.cpp
// never interprets these bytes itself, it only carries whatever is in
// the row out to the decoder, which is where the colour actually comes
// from (confirmed by reading teletext.cpp -- there is no colour
// handling in there at all, only the ETS comment describing what a
// receiver does with it). Everything below is PC-tool side: colour a
// run of characters, and Encode() turns runs into the right lead-in
// bytes when Save sends the row.
//
// CODE 0 (ALPHA BLACK) IS LEFT OUT OF THE PALETTE. The firmware stores
// a row with strncpy() into a plain C string (cfgSetTeletextRow() in
// settings.cpp); a 0x00 byte anywhere in the middle would read as the
// end of the string there and silently truncate the rest of the row.
// Every other alpha code (1..7) is a normal non-zero byte and carries
// through untouched.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace PalSign;

public class TeletextDialog : Form
{
    private const int Cols = 40;
    private const float FontPt = 13f;

    // Index is the ETS 300 706 alpha colour code (1..7); index 0 (black)
    // is never offered, see the file header.
    private static readonly Color[] Palette =
    {
        Color.Black,                  // 0, unused
        Color.Red,                    // 1
        Color.FromArgb(0, 255, 0),    // 2, pure green -- Color.Green is a dark shade
        Color.Yellow,                 // 3
        Color.Blue,                   // 4
        Color.Magenta,                // 5
        Color.Cyan,                   // 6
        Color.White,                  // 7, the default a row starts in
    };
    private static readonly string[] PaletteNames =
        { "", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White" };
    private const int DefaultCode = 7;   // white

    private readonly Func<string?> _port;
    private readonly RichTextBox[] _rows;
    private readonly string[] _original;
    private RichTextBox? _active;

    /// <summary>What actually went to the board, filled in once Save
    /// succeeds; the caller mirrors it into its own BoardSettings.</summary>
    public string[] Result { get; private set; } = Array.Empty<string>();

    public TeletextDialog(Func<string?> port, string[] rows)
    {
        _port = port;
        _original = (string[])rows.Clone();
        _rows = new RichTextBox[rows.Length];

        Text = UiText.TeletextDialogTitle;
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MinimizeBox = false;
        MaximizeBox = false;

        var mono = new Font(FontFamily.GenericMonospace, FontPt);
        Size cell = TextRenderer.MeasureText(new string('0', Cols), mono);
        int rowH = mono.Height + 4;

        var hint = new Label
        {
            Text = UiText.TeletextDialogHint, AutoSize = true, Dock = DockStyle.Top,
            Margin = new Padding(0), Padding = new Padding(10, 8, 10, 2),
        };

        var tips = new ToolTip();
        var palette = new FlowLayoutPanel
        {
            Dock = DockStyle.Top, AutoSize = true, WrapContents = false,
            Padding = new Padding(10, 2, 10, 6),
        };
        for (int code = 1; code < Palette.Length; code++)
        {
            int c = code;
            var swatch = new Panel
            {
                BackColor = Palette[c], Size = new Size(26, 26),
                Margin = new Padding(0, 0, 4, 0), BorderStyle = BorderStyle.FixedSingle,
                Cursor = Cursors.Hand,
            };
            tips.SetToolTip(swatch, PaletteNames[c]);
            swatch.Click += (_, __) => ApplyColor(c);
            palette.Controls.Add(swatch);
        }

        // Same vertical margin on both, or the row's default (3 on every
        // side) versus this one's explicit 0 leaves them a few pixels
        // out of line even though the buttons themselves are the same
        // size.
        var save = new Button { Text = UiText.Save, AutoSize = true, Margin = new Padding(0) };
        var cancel = new Button { Text = UiText.Cancel, AutoSize = true, Margin = new Padding(8, 0, 0, 0) };
        Theme.StyleButton(save, primary: true);
        Theme.StyleButton(cancel);
        save.Click += (_, __) => SaveAndClose();
        cancel.Click += (_, __) => { DialogResult = DialogResult.Cancel; Close(); };
        AcceptButton = save;
        CancelButton = cancel;

        var buttons = new FlowLayoutPanel
        {
            Dock = DockStyle.Bottom, FlowDirection = FlowDirection.RightToLeft,
            AutoSize = true, Padding = new Padding(10),
        };
        buttons.Controls.Add(save);
        buttons.Controls.Add(cancel);

        var screen = new Panel
        {
            BackColor = Color.Black, Dock = DockStyle.Fill, Padding = new Padding(8),
        };
        var grid = new TableLayoutPanel
        {
            ColumnCount = 1, RowCount = rows.Length, Dock = DockStyle.Fill,
            BackColor = Color.Black,
        };
        for (int i = 0; i < rows.Length; i++)
        {
            var box = new RichTextBox
            {
                Font = mono, Dock = DockStyle.Fill, BorderStyle = BorderStyle.None,
                BackColor = Color.Black, ForeColor = Palette[DefaultCode],
                WordWrap = false, ScrollBars = RichTextBoxScrollBars.None,
                DetectUrls = false, ShortcutsEnabled = true,
            };
            Decode(box, rows[i]);
            box.Enter += (_, __) => _active = box;
            int next = i + 1;
            box.KeyDown += (_, e) =>
            {
                if (e.KeyCode != Keys.Enter) return;
                e.SuppressKeyPress = true;   // no newline, no jump to Save
                if (next < _rows.Length) _rows[next].Focus(); else save.Focus();
            };
            box.TextChanged += (_, __) => EnforceBudget(box);
            _rows[i] = box;
            grid.Controls.Add(box, 0, i);
            grid.RowStyles.Add(new RowStyle(SizeType.Absolute, rowH));
        }
        screen.Controls.Add(grid);
        _active = _rows.Length > 0 ? _rows[0] : null;

        Controls.Add(screen);
        Controls.Add(buttons);
        Controls.Add(palette);
        Controls.Add(hint);

        int width = cell.Width + screen.Padding.Horizontal + 24;
        // hint.PreferredHeight is measured before the label has been through
        // a layout pass at this width, so it may wrap to a second line once
        // shown; 44 covers one or two lines of the default UI font either way.
        ClientSize = new Size(
            width,
            44 + 34 + rowH * rows.Length + screen.Padding.Vertical + buttons.PreferredSize.Height + 8);
    }

    // Sets the colour of the current selection in whichever row was last
    // focused (a zero length selection colours what gets typed next, the
    // usual RichTextBox behaviour).
    private void ApplyColor(int code)
    {
        if (_active == null) return;
        _active.SelectionColor = Palette[code];
    }

    // Undoes the last edit if it pushed the row's ENCODED length (visible
    // characters plus one byte per colour change) past Cols. MaxLength
    // alone cannot enforce this, since a colour code costs a byte that
    // is not a visible character.
    private void EnforceBudget(RichTextBox box)
    {
        // Encode() walks the text with Select(), which moves the caret;
        // put it back unless the edit is rejected outright, or every
        // keystroke would leave a stray one-character selection behind.
        int start = box.SelectionStart, len = box.SelectionLength;
        if (Encode(box).Length <= Cols) { box.Select(start, len); return; }
        if (box.CanUndo) { box.Undo(); box.ClearUndo(); }
    }

    // Walks the row's colour runs and turns each one that differs from
    // what a decoder would already be showing into a lead-in code byte
    // plus the run's characters. The very first run needs a code only if
    // it is not white, since white is where a row starts by default.
    private static string Encode(RichTextBox box)
    {
        string text = box.Text;
        var sb = new StringBuilder();
        int prevCode = DefaultCode;
        bool first = true;
        int i = 0;
        while (i < text.Length)
        {
            box.Select(i, 1);
            int code = CodeOf(box.SelectionColor);
            int start = i;
            while (i < text.Length)
            {
                box.Select(i, 1);
                if (CodeOf(box.SelectionColor) != code) break;
                i++;
            }
            bool needsCode = first ? code != DefaultCode : code != prevCode;
            if (needsCode) sb.Append((char)code);
            sb.Append(text, start, i - start);
            prevCode = code;
            first = false;
        }
        return sb.ToString();
    }

    // Rebuilds the coloured runs from a row as the board sent it (or as
    // an empty string, for a row never customised).
    private static void Decode(RichTextBox box, string raw)
    {
        box.Clear();
        Font mono = box.Font;
        int code = DefaultCode;
        var run = new StringBuilder();
        void Flush()
        {
            if (run.Length == 0) return;
            box.SelectionStart = box.TextLength;
            box.SelectionLength = 0;
            box.SelectionColor = Palette[code];
            box.SelectionFont = mono;
            box.AppendText(run.ToString());
            run.Clear();
        }
        foreach (char ch in raw)
        {
            if (ch >= 1 && ch <= 7)
            {
                Flush();
                code = ch;
                continue;
            }
            run.Append(ch);
        }
        Flush();
    }

    private static int CodeOf(Color c)
    {
        for (int i = 1; i < Palette.Length; i++)
            if (Palette[i].ToArgb() == c.ToArgb()) return i;
        return DefaultCode;
    }

    private void SaveAndClose()
    {
        string? p = _port();
        if (p == null)
        {
            MessageBox.Show(this, UiText.NoPortChosen, UiText.Save);
            return;
        }

        var current = new string[_rows.Length];
        for (int i = 0; i < _rows.Length; i++) current[i] = Encode(_rows[i]);

        var changed = new List<(int Row, string Text)>();
        for (int i = 0; i < _rows.Length; i++)
            if (current[i] != _original[i]) changed.Add((i + 1, current[i]));

        if (changed.Count == 0)
        {
            Result = current;
            DialogResult = DialogResult.OK;
            Close();
            return;
        }

        string? error = null;
        using (var d = new ProgressDialog(UiText.SavingTitle, marquee: true))
        {
            d.Report(UiText.TeletextSavingText, -1);
            var worker = new Thread(() =>
            {
                try
                {
                    foreach (var (row, text) in changed)
                        BoardConfig.Set(p, "teletext." + row.ToString(System.Globalization.CultureInfo.InvariantCulture),
                                        text, CancellationToken.None);
                }
                catch (Exception ex) { error = ex.Message; }
                finally
                {
                    try { BeginInvoke(d.Close); }
                    catch (Exception) { /* the window is going */ }
                }
            }) { IsBackground = true };
            worker.Start();
            d.ShowDialog(this);
        }
        if (error != null)
        {
            MessageBox.Show(this, error, UiText.Save);
            return;
        }
        Result = current;
        Session.Dirty = true;
        DialogResult = DialogResult.OK;
        Close();
    }
}

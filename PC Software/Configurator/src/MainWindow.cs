// The window, built in code rather than the designer, so there is no
// .Designer.cs/.resx pair to read in a diff.
//
// Everything sits in a TableLayoutPanel: with WinForms the order of
// Controls.Add decides who wins a dock conflict, and a fixed row layout
// avoids that pitfall.
using System;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Windows.Forms;

namespace PalSign;

public class MainWindow : Form
{
    private readonly Button _save = new()
        { Text = UiText.Save, AutoSize = true, Margin = new Padding(0, 2, 0, 0), Enabled = false };
    private readonly Button _connect = new()
        { Text = UiText.Connect, AutoSize = true, Margin = new Padding(0, 2, 0, 0) };
    // Between Save and Connect on purpose: the one toolbar row that is
    // always on screen, whatever tab is open.
    private readonly CheckBox _alwaysOnTop = new()
        { Text = UiText.AlwaysOnTop, AutoSize = true, Margin = new Padding(12, 6, 12, 0) };
    private readonly System.Windows.Forms.Timer _blink = new() { Interval = 450 };
    private CardsTab _cards = null!;
    private SystemTab _system = null!;
    private SlideshowTab _slides = null!;
    private bool _on;

    // Shared status line, hard right on the tab strip. Fixed width and
    // right aligned so growing text grows leftwards; bold to read as a
    // message rather than a control.
    private readonly Label _status = new()
    {
        AutoSize = false,
        Size = new Size(360, 20),
        TextAlign = ContentAlignment.MiddleRight,
        Font = new Font(Control.DefaultFont, FontStyle.Bold),
    };

    // Not a live socket: says whether the window holds an answer from
    // the board, which decides what the Connect/Disconnect button offers.
    private bool _connected;

    private readonly ComboBox _ports = new()
    {
        DropDownStyle = ComboBoxStyle.DropDownList,
        Width = 130,
        Margin = new Padding(6, 4, 6, 4),
    };

    public MainWindow()
    {
        Text = UiText.AppTitle;
        MinimumSize = new Size(760, 860);
        Size = new Size(780, 980);
        BackColor = Theme.Paper;
        Font = new Font(Font.FontFamily, 9f);
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); }
        catch (Exception) { /* the default .NET icon stands */ }

        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 2,
            BackColor = Theme.Paper,
        };
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

        // A table, not a flow: port left, Save centred, Always on top
        // and Connect hard right of that. Equal outer columns keep Save
        // centred on the window. A panel wraps it for a toolbar-like
        // band, with the accent as a hairline along the bottom rather
        // than a full-strength fill, which would fight with the Save
        // button's own red for warnings.
        var barHost = new Panel
        {
            Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            BackColor = Theme.Panel, Padding = new Padding(12, 10, 12, 9),
        };
        var bar = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, ColumnCount = 3, AutoSize = true,
        };
        bar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        bar.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        bar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        barHost.Controls.Add(bar);
        barHost.Paint += (_, e) =>
        {
            using var pen = new Pen(Theme.Accent, 2);
            e.Graphics.DrawLine(pen, 0, barHost.Height - 1, barHost.Width, barHost.Height - 1);
        };

        var left = new FlowLayoutPanel
            { AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        left.Controls.Add(new Label
            { Text = UiText.PortLabel, AutoSize = true, ForeColor = Theme.Ink, Margin = new Padding(0, 8, 4, 0) });
        left.Controls.Add(_ports);
        var search = new Button { Text = UiText.Search, AutoSize = true, Margin = new Padding(6, 2, 0, 0) };
        Theme.StyleButton(search);
        search.Click += (_, _) => RefreshPorts();
        left.Controls.Add(search);
        bar.Controls.Add(left, 0, 0);

        Theme.StyleButton(_save, primary: false);   // Blink() takes over its colours once dirty
        _save.Anchor = AnchorStyles.None;          // centred in its cell
        bar.Controls.Add(_save, 1, 0);

        // Always on top and Connect share the right cell instead of a
        // fourth grid column, so the two Percent(50) columns either
        // side of Save keep exactly the balance they had (see the note
        // above): only what is inside the right cell changes.
        var right = new FlowLayoutPanel
            { Dock = DockStyle.Right, AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        var about = new Button { Text = UiText.About, AutoSize = true, Margin = new Padding(0, 2, 12, 0) };
        Theme.StyleButton(about);
        about.Click += (_, _) => AboutDialog.ShowAbout(this, UiText.AboutAppName);
        right.Controls.Add(about);
        _alwaysOnTop.Margin = new Padding(0, 8, 12, 0);
        right.Controls.Add(_alwaysOnTop);
        Theme.StyleButton(_connect, primary: true);
        _connect.Margin = new Padding(0);
        right.Controls.Add(_connect);
        bar.Controls.Add(right, 2, 0);

        _alwaysOnTop.CheckedChanged += (_, _) => TopMost = _alwaysOnTop.Checked;
        // A TopMost window sits in its own z-order band above every
        // ordinary window, and Windows does not exempt that window's
        // own dialogs from it: MessageBox, OpenFileDialog, ColorDialog,
        // ProgressDialog and the rest are ordinary windows, so a
        // TopMost main window can cover them even while they are
        // modal. Dropping TopMost the moment any of them takes the
        // focus, and picking it back up once this window has it again,
        // fixes every one of them from here, rather than needing a
        // TopMost of its own at each of the forty-odd places a dialog
        // opens.
        Deactivate += (_, _) => TopMost = false;
        Activated += (_, _) => TopMost = _alwaysOnTop.Checked;
        _save.Click += (_, _) => SaveToFlash();
        _connect.Click += (_, _) =>
        {
            if (_connected)
            {
                if (_slides.Dirty && MessageBox.Show(this, UiText.SlideshowUnwrittenAsk,
                                                     UiText.SlideshowUnwrittenTitle,
                                                     MessageBoxButtons.YesNo,
                                                     MessageBoxIcon.Warning) != DialogResult.Yes)
                    return;
                if (Session.Dirty)
                {
                    var choice = MessageBox.Show(this, UiText.UnsavedAsk, UiText.UnsavedTitle,
                                                  MessageBoxButtons.YesNoCancel, MessageBoxIcon.Warning);
                    if (choice == DialogResult.Cancel) return;
                    if (choice == DialogResult.Yes)
                    {
                        SaveToFlash();
                        if (Session.Dirty) return;   // the save itself failed and already said so
                    }
                }
                _cards.Disconnect();
                _system.Forget();
                _slides.Forget();
            }
            else _cards.ConnectNow();
        };
        _blink.Tick += (_, _) => Blink();
        Blink();   // the disabled look applies from the first frame, not the first tick
        _blink.Start();

        _cards = new CardsTab(() => _ports.SelectedItem as string);
        var system = new SystemTab(() => _ports.SelectedItem as string);
        _system = system;
        // One read for the whole window: Connect hands the same answer
        // to System and Slideshow, so neither reads the board again.
        var slides = new SlideshowTab(() => _ports.SelectedItem as string);
        _slides = slides;
        slides.Save = () => SaveToFlash();
        // So the list and the preview on the Cards tab follow a
        // slideshow the board is stepping through on its own.
        slides.RunningChanged = _cards.SetSlideshowRunning;
        _cards.Settings = cfg => { system.Adopt(cfg); slides.Adopt(cfg); };
        _cards.Live = on =>
        {
            _connected = on;
            _connect.Text = on ? UiText.Disconnect : UiText.Connect;
        };
        // A shared setting can put CardsTab's pictures out of date; it
        // catches up at Connect and after a save.
        system.Changed = _cards.NoteSharedChange;
        system.FetchAll = _cards.FetchAllAgain;
        system.UpdateFirmware = UpdateFirmware;
        var tabs = new TabControl
            { Dock = DockStyle.Fill, DrawMode = TabDrawMode.OwnerDrawFixed, Padding = new Point(16, 8) };
        tabs.TabPages.Add(_cards);
        tabs.TabPages.Add(slides);
        tabs.TabPages.Add(system);
        // WinForms gives no style hook for the tab strip itself, only
        // this per-item draw callback: fill flat, then an accent
        // underline on the selected tab in place of the 3D folder edge.
        tabs.DrawItem += (_, e) =>
        {
            var g = e.Graphics;
            bool selected = e.Index == tabs.SelectedIndex;
            using (var bg = new SolidBrush(Theme.Paper))
                g.FillRectangle(bg, e.Bounds);
            var page = tabs.TabPages[e.Index];
            using var font = new Font(tabs.Font, selected ? FontStyle.Bold : FontStyle.Regular);
            TextRenderer.DrawText(g, page.Text, font, e.Bounds, selected ? Theme.Ink : Theme.Muted,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            if (selected)
                using (var pen = new Pen(Theme.Accent, 3))
                    g.DrawLine(pen, e.Bounds.Left + 6, e.Bounds.Bottom - 3, e.Bounds.Right - 6, e.Bounds.Bottom - 3);
        };

        // The status label is added first so WinForms paints it on top;
        // it sits over the dead space right of the tab strip. Program.cs's
        // layout check knows this exception.
        var host = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Paper, Padding = new Padding(10) };
        host.Controls.Add(_status);
        host.Controls.Add(tabs);
        host.Layout += (_, __) =>
        {
            int strip = tabs.DisplayRectangle.Top;   // the pages start here
            _status.SetBounds(host.ClientSize.Width - _status.Width - 10,
                              Math.Max(0, (strip - _status.Height) / 2),
                              _status.Width, _status.Height);
        };
        _cards.Status = s => _status.Text = s;
        _system.Status = s => _status.Text = s;

        root.Controls.Add(barHost, 0, 0);
        root.Controls.Add(host, 0, 1);
        Controls.Add(root);

        RefreshPorts();

        // The version, briefly, at startup; the About button keeps it
        // reachable. Not in the headless --layout/--screenshot paths,
        // which Show() the window without a message loop.
        Shown += (_, _) => { if (Application.MessageLoop) AboutDialog.Splash(this, UiText.AboutAppName); };
    }

    // Red and blinking while there is something unsaved to flash, grey
    // otherwise, so a change is never lost silently. Sets BackColor/
    // ForeColor/FlatAppearance directly rather than through
    // UseVisualStyleBackColor, which FlatStyle.Flat does not honour.
    private void Blink()
    {
        if (!Session.Dirty)
        {
            _save.Enabled = false;
            Theme.RefreshButtonEnabled(_save);
            _on = false;
            return;
        }
        _save.Enabled = true;
        _on = !_on;
        _save.BackColor = _on ? Color.FromArgb(220, 40, 40) : Theme.Panel;
        _save.ForeColor = _on ? Color.White : Color.FromArgb(180, 30, 30);
        _save.FlatAppearance.BorderColor = Color.FromArgb(180, 30, 30);
    }

    private void SaveToFlash()
    {
        string? p = _ports.SelectedItem as string;
        if (p == null)
        {
            _status.Text = UiText.NoPortChosen;
            return;
        }
        _save.Enabled = false;
        Cursor = Cursors.AppStarting;
        // The tabs hold the port open briefly after a drag/type; release
        // it here or Save's own port open fails with "access denied".
        _cards.ReleasePort();
        _system.ReleasePort();
        string? error = null;
        using (var d = new ProgressDialog(UiText.SavingTitle, marquee: true))
        {
            d.Report(UiText.SavingText, -1);
            var worker = new System.Threading.Thread(() =>
            {
                try { BoardConfig.Save(p, System.Threading.CancellationToken.None); }
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
        if (error != null) MessageBox.Show(this, error, UiText.Save);
        else
        {
            Session.Dirty = false;
            _cards.RefreshAfterSave();   // the other check point besides Connect
        }
        Cursor = Cursors.Default;
    }

    private void UpdateFirmware()
    {
        string? p = _ports.SelectedItem as string;
        if (p == null)
        {
            _status.Text = UiText.NoPortChosen;
            return;
        }
        using var open = new OpenFileDialog
            { Filter = UiText.FirmwareUpdateFilter, Title = UiText.FirmwareUpdateTitle };
        if (open.ShowDialog(this) != DialogResult.OK) return;
        string uf2 = open.FileName;

        if (MessageBox.Show(this, string.Format(UiText.FirmwareUpdateAskFmt, Path.GetFileName(uf2)),
                            UiText.FirmwareUpdateTitle, MessageBoxButtons.YesNo, MessageBoxIcon.Warning)
            != DialogResult.Yes) return;

        // Same reason as Save: a tab holding the port from a recent
        // drag/type would otherwise make the port open below fail.
        _cards.ReleasePort();
        _system.ReleasePort();
        string? error = null;
        using (var d = new ProgressDialog(UiText.FirmwareUpdateTitle, marquee: true))
        {
            d.Report(UiText.FirmwareUpdateButton, -1);
            var worker = new Thread(() =>
            {
                try
                {
                    BoardFirmwareUpdate.Send(p, uf2, line =>
                    {
                        try { BeginInvoke(() => d.Report(line, -1)); }
                        catch (Exception) { /* the window is going */ }
                    }, CancellationToken.None);
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
        if (error != null) MessageBox.Show(this, error, UiText.FirmwareUpdateTitle);
        else MessageBox.Show(this, UiText.FirmwareUpdateDone, UiText.FirmwareUpdateTitle);
    }

    private void RefreshPorts()
    {
        string? current = _ports.SelectedItem as string;
        _ports.Items.Clear();
        foreach (string p in BoardUpload.Ports()) _ports.Items.Add(p);
        if (_ports.Items.Count > 0)
            _ports.SelectedItem = current != null && _ports.Items.Contains(current)
                ? current : _ports.Items[0];
    }
}

/// <summary>Choose a photo, set it up, view it and upload it. Opens from
/// the card it belongs to, so the slot is already known.
///
/// A UserControl, not a TabPage: WinForms refuses to add a TabPage
/// anywhere but a TabControl.</summary>
public class PhotoPanel : UserControl
{
    /// <summary>Opens on a slot, counted from 1.</summary>
    public void PickSlot(int slot)
    {
        if (slot >= 1 && slot <= _slot.Items.Count) _slot.SelectedIndex = slot - 1;
    }

    // 4:3 at 720x576 is 768 wide; the preview keeps that ratio, because
    // that is how the picture arrives on the television.
    private const int PreviewW = 448;
    private const int PreviewH = PreviewW * ImageConvert.ScreenH / 768;

    private readonly Func<string?> _port;
    private readonly string _cardName;
    // Whether Upload() should confirm an overwrite; fixed for this slot,
    // since _slot below is never user-editable.
    private readonly bool _alreadyFilled;
    private readonly Label _file = new() { Text = UiText.NoPhotoChosen, AutoSize = true, Margin = new Padding(10, 8, 0, 0) };
    private readonly ComboBox _slot = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 110, Margin = new Padding(0, 2, 0, 0) };
    private readonly CheckBox _fill = new() { Text = UiText.CropUntilFills, AutoSize = true, Margin = new Padding(0, 6, 0, 0) };
    private readonly CheckBox _square = new() { Text = UiText.ForceSquarePixels, AutoSize = true, Margin = new Padding(0, 4, 0, 6) };
    private readonly AccentSlider _flicker = new() { Minimum = 0, Maximum = 100, Value = 100, TickFrequency = 25, Width = 280, Margin = new Padding(0, 0, 8, 0) };
    private readonly Label _flickerLabel = new() { AutoSize = true, Margin = new Padding(0, 10, 0, 0) };
    private readonly PictureBox _canvas = new()
    {
        Width = PreviewW,
        Height = PreviewH,
        BorderStyle = BorderStyle.FixedSingle,
        SizeMode = PictureBoxSizeMode.StretchImage,
        Anchor = AnchorStyles.Top,
        Margin = new Padding(0, 8, 0, 4),
    };
    private readonly Label _size = new() { AutoSize = true, Anchor = AnchorStyles.Top, Margin = new Padding(0, 0, 0, 8) };
    private readonly Button _upload = new() { Text = UiText.UploadButton, AutoSize = true, Enabled = false, Margin = new Padding(0, 0, 10, 0) };

    /// <summary>Fired after a successful upload, with the 4:2:2 bytes
    /// and the slot they went to; the board holds exactly these bytes,
    /// so CardsTab keeps them instead of fetching back.</summary>
    public event Action<byte[], int>? Uploaded;

    private string? _path;
    private byte[]? _bytes;

    public PhotoPanel(Func<string?> port, string cardName, bool alreadyFilled)
    {
        _port = port;
        _cardName = cardName;
        _alreadyFilled = alreadyFilled;
        BackColor = SystemColors.Control;
        Padding = new Padding(12);

        var table = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 5,
        };
        for (int i = 0; i < 5; i++) table.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        // --- row 0: choose a file -----------------------------------
        var chooseRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, 10) };
        var choose = new Button { Text = UiText.ChoosePhoto, AutoSize = true, Padding = new Padding(8, 2, 8, 2) };
        choose.Click += (_, _) => Choose();
        chooseRow.Controls.Add(choose);
        chooseRow.Controls.Add(_file);
        table.Controls.Add(chooseRow, 0, 0);

        // --- row 1: settings ----------------------------------------
        var box = new GroupBox
        {
            Text = UiText.SettingsGroup,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Dock = DockStyle.Fill,
            Padding = new Padding(12, 8, 12, 12),
            Margin = new Padding(0, 0, 0, 10),
        };
        var settings = new TableLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 1,
            Dock = DockStyle.Top,
        };

        // _slot is deliberately never added to a visible container: the
        // window opens with the slot already known (PickSlot()), and it
        // stays fixed for the life of the dialog.
        for (int i = 1; i <= 14; i++) _slot.Items.Add(string.Format(UiText.CustomSlotFmt, i));
        _slot.SelectedIndex = 0;

        _fill.CheckedChanged += (_, _) => Process();
        _square.CheckedChanged += (_, _) => Process();
        settings.Controls.Add(_fill);
        settings.Controls.Add(_square);

        var flickerRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 4, 0, 0) };
        flickerRow.Controls.Add(new Label { Text = UiText.FlickerLabel, AutoSize = true, Margin = new Padding(0, 10, 12, 0) });
        _flicker.Scroll += (_, _) => Process();
        flickerRow.Controls.Add(_flicker);
        flickerRow.Controls.Add(_flickerLabel);
        settings.Controls.Add(flickerRow);

        box.Controls.Add(settings);
        table.Controls.Add(box, 0, 1);

        // --- rows 2 and 3: preview ----------------------------------
        table.Controls.Add(_canvas, 0, 2);
        table.Controls.Add(_size, 0, 3);

        // --- row 4: uploading ---------------------------------------
        var uploadRow = new FlowLayoutPanel
            { AutoSize = true, WrapContents = false, Margin = new Padding(0, 4, 0, 0) };
        _upload.Click += (_, _) => Upload();
        uploadRow.Controls.Add(_upload);
        table.Controls.Add(uploadRow, 0, 4);

        Controls.Add(table);
        UpdateFlickerLabel();
    }

    private double FlickerStrength => _flicker.Value / 100.0;

    private void UpdateFlickerLabel()
    {
        double v = FlickerStrength;
        string explanation = v > 0.95 ? UiText.FlickerFull
                           : v < 0.05 ? UiText.FlickerOff
                           : UiText.FlickerPartial;
        _flickerLabel.Text = $"{v:0.00}  {explanation}";
    }

    private void Report(string text) => Trace.Say(text);

    private void Choose()
    {
        using var dlg = new OpenFileDialog
        {
            Title = "Choose photo",
            Filter = UiText.ImagesFilter,
        };
        if (dlg.ShowDialog(this) != DialogResult.OK) return;
        _path = dlg.FileName;
        _file.Text = Path.GetFileName(_path);
        Process();
    }

    private void Process()
    {
        UpdateFlickerLabel();
        if (_path == null) return;
        try
        {
            double[,,] rgb = ImageReader.Read(_path);
            int srcW = rgb.GetLength(1), srcH = rgb.GetLength(0);
            double[,,] canvas = ImageConvert.FitIn(rgb, _fill.Checked, _square.Checked);
            _bytes = ImageConvert.To422(canvas, FlickerStrength);

            // Shown at screen ratio, not the raster: 720x576 is anamorphic.
            Image? old = _canvas.Image;
            _canvas.Image = ImageReader.ToBitmap(_bytes, ImageConvert.ScreenW, ImageConvert.ScreenH);
            old?.Dispose();
            _size.Text = string.Format(UiText.SourceShownFmt, srcW, srcH);
            _upload.Enabled = true;
        }
        catch (Exception e)
        {
            MessageBox.Show(this, UiText.CannotProcess + e.Message, UiText.AppTitle,
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void Upload()
    {
        string? port = _port();
        if (string.IsNullOrEmpty(port))
        {
            MessageBox.Show(this, UiText.ChoosePortFirst, UiText.AppTitle);
            return;
        }
        if (_bytes == null || _path == null) return;
        if (_alreadyFilled && MessageBox.Show(this, string.Format(UiText.UploadOverwriteAskFmt, _cardName),
                                              UiText.UploadOverwriteTitle, MessageBoxButtons.OKCancel,
                                              MessageBoxIcon.Warning) != DialogResult.OK)
            return;

        _upload.Enabled = false;
        Report($"--- upload to {port} ---");
        Report("Note: no picture comes out while writing, but the board does not restart");
        Report("and the port stays open. See photoUpload() in Firmware/Firmware.ino.");

        byte[] data = _bytes;
        string name = Path.GetFileName(_path);
        int slot = _slot.SelectedIndex + 1;

        // Modal progress window; worker thread since a message pump in
        // the write loop would make the board give up (see BoardUpload).
        string? error = null;
        using (var d = new ProgressDialog(UiText.UploadingTitle))
        {
            d.Report(string.Format(UiText.UploadingFmt, name), 0);
            // The erase (9-82s, silent) fakes 0..50; sector acks fill
            // the real 50..100.
            d.FakePace(50, 200);
            var worker = new Thread(() =>
            {
                try
                {
                    BoardUpload.Send(port, data, name, slot,
                        t => Guarded(() => Report(t)),
                        (n, total) => Guarded(() =>
                            d.Report(null, total > 0 ? 50 + (int)(n * 50L / total) : 0)),
                        CancellationToken.None);
                }
                catch (Exception e) { error = e.Message; }
                finally { Guarded(d.Close); }
            })
            { IsBackground = true };
            worker.Start();
            d.ShowDialog(FindForm());
        }
        _upload.Enabled = true;
        if (error != null)
        {
            Report("FAILED: " + error);
            MessageBox.Show(FindForm(), error, UiText.AppTitle,
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        Report("Done.");
        Uploaded?.Invoke(data, slot);
    }

    private void Guarded(Action what)
    {
        try { BeginInvoke(what); }
        catch (Exception) { /* the window is going */ }
    }
}

// One photo, centered once, uploaded into both BBC Test Card F and W's
// own oval photo window. The two windows are not the same shape (302
// rows for F, 310 for W; see PortraitGeometry.g.cs, generated from the
// firmware's own tables), so the single crop the user picks here is
// converted to full 720x576 4:2:2 through the same ImageConvert path
// every Custom photo goes through, and each oval's bytes are then cut
// straight out of that one converted picture: no separate colour math
// for the oval, so the two can never drift the way the C#/Python
// arithmetic itself is guarded against in compare.py.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Threading;
using System.Windows.Forms;

namespace PalSign;

public class PortraitDialog : Form
{
    private readonly Func<string?> _port;

    private const int PreviewW = 448;
    private const int PreviewH = PreviewW * ImageConvert.ScreenH / ImageConvert.ScreenW;

    private readonly Label _file = new()
        { Text = UiText.NoPhotoChosen, AutoSize = true, Margin = new Padding(10, 8, 0, 0) };
    private readonly OvalPreview _canvas = new()
        { Width = PreviewW, Height = PreviewH, Margin = new Padding(0, 8, 0, 8), Cursor = Cursors.SizeAll };
    // 1.0 (the minimum) shows the whole photo, letterboxed if its shape
    // does not match 720x576; nothing is cropped away until the user
    // zooms in. 20x at the top: the oval is small next to a whole
    // photo, and a face often needs to fill most of it. TickFrequency
    // in the hundreds, or AccentSlider draws a tick every single value
    // on a range this wide.
    private readonly AccentSlider _zoom = new()
        { Minimum = 100, Maximum = 2000, Value = 100, TickFrequency = 100, Width = 260 };
    private readonly Button _upload = new()
        { Text = UiText.UploadButton, AutoSize = true, Enabled = false };

    private string? _path;
    private int _srcW, _srcH;

    // The scale (screen pixels per source pixel) at Zoom == 1.0, the
    // whole photo visible letterboxed inside 720x576; set once the
    // photo is loaded, see Choose(). Zoom multiplies this, never
    // replaces it, so 1.0 always means "everything, nothing cropped
    // away yet" regardless of the source picture's own shape.
    private double _containScale = 1.0;

    // Pan: the source pixel that sits at the centre of the view.
    // Dragging on _canvas is the only way to change these; set to the
    // photo's own centre once it is chosen.
    private int _panX, _panY;
    private bool _dragging;
    private Point _dragFrom;

    public PortraitDialog(Func<string?> port)
    {
        _port = port;
        Text = UiText.UploadPortraitTitle;
        Size = new Size(760, 720);
        MinimumSize = new Size(640, 600);
        StartPosition = FormStartPosition.CenterParent;
        MinimizeBox = false;
        BackColor = SystemColors.Control;
        Padding = new Padding(12);
        Theme.StyleButton(_upload, primary: true);

        var table = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5 };
        for (int i = 0; i < 5; i++) table.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var chooseRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, 4) };
        var choose = new Button { Text = UiText.ChoosePhoto, AutoSize = true, Padding = new Padding(8, 2, 8, 2) };
        Theme.StyleButton(choose);
        choose.Click += (_, __) => Choose();
        chooseRow.Controls.Add(choose);
        chooseRow.Controls.Add(_file);
        table.Controls.Add(chooseRow, 0, 0);

        var explain = new Label
        {
            Text = UiText.PortraitExplain,
            AutoSize = true,
            ForeColor = Theme.Muted,
            Margin = new Padding(0, 0, 0, 8),
        };
        table.Controls.Add(explain, 0, 1);

        // Drag the photo itself to pan; the drop point is not meant to
        // matter, only the distance moved since the last event, so
        // every step just moves _dragFrom along with it. Capture is
        // explicit: a plain Panel does not take it on its own, so a
        // fast drag that carries the cursor outside _canvas would
        // otherwise stop delivering MouseMove, and even MouseUp, and
        // dragging would not work at all rather than just feel jerky.
        _canvas.MouseDown += (_, e) => { _dragging = true; _dragFrom = e.Location; _canvas.Capture = true; };
        _canvas.MouseUp += (_, __) => { _dragging = false; _canvas.Capture = false; };
        _canvas.MouseMove += (_, e) =>
        {
            if (!_dragging || _srcW == 0) return;
            int dx = e.Location.X - _dragFrom.X, dy = e.Location.Y - _dragFrom.Y;
            _dragFrom = e.Location;
            if (dx == 0 && dy == 0) return;
            // Preview pixels -> pixels of the zoomed 720x576 view -> pixels
            // of the source photo, which is what _panX/_panY are in.
            // Dragging right/down has to move the PICTURE right/down, so
            // the view (what Build() reads out of the source) moves the
            // other way.
            double toSource = ImageConvert.ScreenW / (double)PreviewW / (_containScale * Zoom);
            _panX -= (int)Math.Round(dx * toSource);
            _panY -= (int)Math.Round(dy * toSource);
            Redraw();
        };

        table.Controls.Add(_canvas, 0, 2);

        var sliders = new TableLayoutPanel { AutoSize = true, ColumnCount = 2, Margin = new Padding(0, 8, 0, 8) };
        sliders.Controls.Add(new Label { Text = UiText.PortraitZoom, AutoSize = true, Margin = new Padding(0, 8, 8, 0) });
        _zoom.Scroll += (_, __) => Redraw();
        sliders.Controls.Add(_zoom);
        table.Controls.Add(sliders, 0, 3);

        var uploadRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false };
        _upload.Click += (_, __) => Upload();
        uploadRow.Controls.Add(_upload);
        table.Controls.Add(uploadRow, 0, 4);

        Controls.Add(table);
    }

    private double Zoom => _zoom.Value / 100.0;

    private void Choose()
    {
        using var dlg = new OpenFileDialog { Title = UiText.ChoosePhoto, Filter = UiText.ImagesFilter };
        if (dlg.ShowDialog(this) != DialogResult.OK) return;
        _path = dlg.FileName;
        _file.Text = Path.GetFileName(_path);
        _upload.Enabled = true;

        double[,,] rgb = ImageReader.Read(_path);
        _srcH = rgb.GetLength(0);
        _srcW = rgb.GetLength(1);
        // The whole photo visible at Zoom == 1.0, letterboxed if its
        // shape does not match 720x576 -- min, not FitIn's own max
        // ("fill"): nothing is cropped away before the user gets a say,
        // unlike a Custom photo upload, where cropping is the point.
        _containScale = Math.Min(ImageConvert.ScreenW / (double)_srcW, ImageConvert.ScreenH / (double)_srcH);
        _panX = _srcW / 2;
        _panY = _srcH / 2;
        _zoom.Value = _zoom.Minimum;
        Redraw();
    }

    /// <summary>The crop the user is looking at: a window over the
    /// original photo, sized by Zoom (1.0 = the whole photo) and moved
    /// by _panX/_panY, resampled up or down to 720x576 with the same
    /// Lanczos filter every Custom photo uses. Parts of the window that
    /// fall outside the photo (zoomed out past 1.0, or panned off an
    /// edge) come out black, the same as FitIn's own letterbox bars.</summary>
    private double[,,]? Build()
    {
        if (_path == null || _srcW == 0) return null;
        double[,,] rgb = ImageReader.Read(_path);
        double scale = _containScale * Zoom;

        int winW = Math.Max(1, (int)Math.Round(ImageConvert.ScreenW / scale));
        int winH = Math.Max(1, (int)Math.Round(ImageConvert.ScreenH / scale));
        int x0 = _panX - winW / 2;
        int y0 = _panY - winH / 2;

        var window = new double[winH, winW, 3];   // defaults to black
        int sx0 = Math.Max(0, x0), sx1 = Math.Min(_srcW, x0 + winW);
        int sy0 = Math.Max(0, y0), sy1 = Math.Min(_srcH, y0 + winH);
        for (int y = sy0; y < sy1; y++)
            for (int x = sx0; x < sx1; x++)
                for (int k = 0; k < 3; k++)
                    window[y - y0, x - x0, k] = rgb[y, x, k];
        return Lanczos.Scale(window, ImageConvert.ScreenW, ImageConvert.ScreenH);
    }

    private void Redraw()
    {
        double[,,]? rgb = Build();
        _canvas.Picture = rgb == null ? null : ImageReader.ToBitmap(
            ImageConvert.To422(rgb, 1.0), ImageConvert.ScreenW, ImageConvert.ScreenH);
    }

    /// <summary>Row r's own (length[r]*2) bytes, cut straight out of the
    /// full 4:2:2 picture at (Y0+r)*1440 + x0[r]*2; see portrait.h in
    /// the firmware for why concatenating rows in order reproduces its
    /// off[] table exactly, with no gaps to leave out.</summary>
    private static byte[] Extract(byte[] full, PortraitOval oval)
    {
        int total = 0;
        for (int r = 0; r < oval.Rows; r++) total += oval.Length[r] * 2;
        var packed = new byte[total];
        int at = 0;
        for (int r = 0; r < oval.Rows; r++)
        {
            int rowStart = (oval.Y0 + r) * ImageConvert.ScreenW * 2 + oval.X0[r] * 2;
            int n = oval.Length[r] * 2;
            Array.Copy(full, rowStart, packed, at, n);
            at += n;
        }
        return packed;
    }

    private void Upload()
    {
        string? port = _port();
        if (string.IsNullOrEmpty(port))
        {
            MessageBox.Show(this, UiText.ChoosePortFirst, UiText.AppTitle);
            return;
        }
        double[,,]? rgb = Build();
        if (rgb == null) return;
        byte[] full = ImageConvert.To422(rgb, 1.0);
        byte[] f = Extract(full, PortraitGeometry.TestCardF);
        byte[] w = Extract(full, PortraitGeometry.TestCardW);

        _upload.Enabled = false;
        Trace.Say($"--- upload portrait to {port} ---");

        Exception? error = null;
        using (var d = new ProgressDialog(UiText.UploadingTitle))
        {
            d.Report(UiText.UploadingPortraitF, 0);
            var worker = new Thread(() =>
            {
                try
                {
                    BoardUpload.SendPortrait(port, f, 0, t => Guarded(() => Trace.Say(t)),
                        (n, total) => Guarded(() => d.Report(null, total > 0 ? (int)(n * 50L / total) : 0)),
                        CancellationToken.None);
                    Guarded(() => d.Report(UiText.UploadingPortraitW, 50));
                    BoardUpload.SendPortrait(port, w, 1, t => Guarded(() => Trace.Say(t)),
                        (n, total) => Guarded(() => d.Report(null, total > 0 ? 50 + (int)(n * 50L / total) : 50)),
                        CancellationToken.None);
                }
                catch (Exception e) { error = e; }
                finally { Guarded(d.Close); }
            })
            { IsBackground = true };
            worker.Start();
            d.ShowDialog(FindForm());
        }
        _upload.Enabled = true;
        if (error != null)
        {
            Trace.Say("FAILED: " + error.Message);
            MessageBox.Show(FindForm(), error.Message, UiText.AppTitle,
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        Trace.Say("Done.");
        MessageBox.Show(FindForm(), UiText.PortraitUploadDone, UiText.AppTitle,
                        MessageBoxButtons.OK, MessageBoxIcon.Information);
        DialogResult = DialogResult.OK;
        Close();
    }

    private void Guarded(Action what)
    {
        try { BeginInvoke(what); }
        catch (Exception) { /* the window is going */ }
    }
}

/// <summary>The 720x576 preview with both ovals outlined on top, so
/// centering for one card does not have to be guessed against the
/// other.</summary>
public class OvalPreview : Panel
{
    public Image? Picture
    {
        get => _picture;
        set { _picture?.Dispose(); _picture = value; Invalidate(); }
    }
    private Image? _picture;

    public OvalPreview()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                 ControlStyles.OptimizedDoubleBuffer, true);
        BorderStyle = BorderStyle.FixedSingle;
    }

    private static readonly Color FColor = Color.FromArgb(0, 140, 255);
    private static readonly Color WColor = Color.FromArgb(255, 140, 0);

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(SystemColors.ControlDark);
        if (_picture != null) g.DrawImage(_picture, ClientRectangle);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        DrawOval(g, PortraitGeometry.TestCardF, FColor);
        DrawOval(g, PortraitGeometry.TestCardW, WColor);
    }

    private void DrawOval(Graphics g, PortraitOval oval, Color color)
    {
        float sx = Width / (float)ImageConvert.ScreenW;
        float sy = Height / (float)ImageConvert.ScreenH;
        var left = new PointF[oval.Rows];
        var right = new PointF[oval.Rows];
        for (int r = 0; r < oval.Rows; r++)
        {
            float y = (oval.Y0 + r) * sy;
            left[r] = new PointF(oval.X0[r] * sx, y);
            right[r] = new PointF((oval.X0[r] + oval.Length[r]) * sx, y);
        }
        using var pen = new Pen(color, 2f);
        g.DrawLines(pen, left);
        g.DrawLines(pen, right);
    }
}

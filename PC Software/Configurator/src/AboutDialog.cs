// The About box for both PAL-sign programs (the PCM decoder links this
// file the way it links Theme.cs): the program's name, the shared
// version and where it comes from. Splash() shows the same box briefly
// at startup and closes it by itself.
using System;
using System.Drawing;
using System.Windows.Forms;

namespace PalSign;

public sealed class AboutDialog : Form
{
    // One version for the whole PC side: the configurator and the PCM
    // decoder ship together from the same repository, so they carry
    // the same number.
    public const string Version = "1.2";
    public const string VersionDate = "11 October 2026";

    // What the connected board reported, for the About box; null when no
    // board is connected.
    public static string? BoardFirmware;

    private AboutDialog(string app, bool splash)
    {
        Text = "About";
        BackColor = Theme.Panel;
        Font = new Font("Segoe UI", 9f);
        FormBorderStyle = splash ? FormBorderStyle.None : FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(400, splash ? 150 : 220);

        var name = new Label
        {
            Text = app, AutoSize = true, ForeColor = Theme.Ink,
            Font = new Font("Segoe UI Semibold", 16f), Location = new Point(24, 20),
        };
        var ver = new Label
        {
            Text = $"Version {Version}  ·  {VersionDate}", AutoSize = true,
            ForeColor = Theme.Accent, Location = new Point(26, 60),
        };
        var who = new Label
        {
            Text = "Part of the PAL-sign test pattern generator\nby Sjef Verhoeven PE5PVB",
            AutoSize = true, ForeColor = Theme.Muted, Location = new Point(26, 92),
        };
        Controls.Add(name);
        Controls.Add(ver);
        Controls.Add(who);
        if (!splash && BoardFirmware != null)
        {
            Controls.Add(new Label
            {
                Text = "Board firmware: " + BoardFirmware,
                AutoSize = true, ForeColor = Theme.Muted, Location = new Point(26, 136),
            });
        }

        if (splash)
        {
            // A borderless box needs its own edge, or it reads as a
            // paint glitch on top of the window behind it.
            Paint += (_, e) =>
            {
                using var pen = new Pen(Theme.Accent, 2);
                e.Graphics.DrawRectangle(pen, 1, 1, ClientSize.Width - 3, ClientSize.Height - 3);
            };
            var t = new Timer { Interval = 2500 };
            t.Tick += (_, __) => { t.Stop(); Close(); };
            t.Start();
            Click += (_, __) => Close();
        }
        else
        {
            var ok = new Button
            {
                Text = "OK", Size = new Size(84, 30),
                Location = new Point(ClientSize.Width - 108, ClientSize.Height - 46),
            };
            Theme.StyleButton(ok, primary: true);
            ok.Click += (_, __) => Close();
            Controls.Add(ok);
            AcceptButton = ok;
            CancelButton = ok;
        }
    }

    public static void ShowAbout(IWin32Window owner, string app)
    {
        using var d = new AboutDialog(app, splash: false);
        d.ShowDialog(owner);
    }

    // Shown briefly at startup and gone by itself; a click dismisses it
    // sooner. Not modal, so the window behind it is usable at once.
    public static void Splash(Form owner, string app)
    {
        var d = new AboutDialog(app, splash: true);
        d.FormClosed += (_, __) => d.Dispose();
        d.Show(owner);
        // CenterParent only works under ShowDialog(); placed by hand.
        d.Location = new Point(owner.Left + (owner.Width - d.Width) / 2,
                               owner.Top + (owner.Height - d.Height) / 2);
    }
}

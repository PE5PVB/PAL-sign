// Reading an image in as [h,w,3] doubles in 0..255. Reading only, no
// scaling: System.Drawing's filter differs from PIL's, so scaling
// happens in Lanczos.cs instead.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace PalSign;

public static class ImageReader
{
    public static double[,,] Read(string path)
    {
        using var bmp = new Bitmap(path);
        return ToArray(bmp);
    }

    public static double[,,] ToArray(Bitmap src)
    {
        int w = src.Width, h = src.Height;
        using var copy = new Bitmap(w, h, PixelFormat.Format24bppRgb);
        using (var g = Graphics.FromImage(copy))
        {
            // Not DrawImageUnscaled: it scales to physical size by dpi.
            // An explicit pixel rectangle goes pixel for pixel instead.
            g.PixelOffsetMode = PixelOffsetMode.Half;
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.DrawImage(src, new Rectangle(0, 0, w, h), new Rectangle(0, 0, w, h),
                        GraphicsUnit.Pixel);
        }

        var outp = new double[h, w, 3];
        BitmapData d = copy.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly,
                                     PixelFormat.Format24bppRgb);
        try
        {
            var row = new byte[Math.Abs(d.Stride)];
            for (int y = 0; y < h; y++)
            {
                Marshal.Copy(d.Scan0 + y * d.Stride, row, 0, row.Length);
                for (int x = 0; x < w; x++)
                {
                    // 24bppRgb sits in memory as B G R.
                    outp[y, x, 2] = row[x * 3];
                    outp[y, x, 1] = row[x * 3 + 1];
                    outp[y, x, 0] = row[x * 3 + 2];
                }
            }
        }
        finally { copy.UnlockBits(d); }
        return outp;
    }

    /// <summary>Decodes the 4:2:2 bytes back into a Bitmap, so the chroma
    /// halving and level limiting show in the preview too.</summary>
    public static Bitmap ToBitmap(byte[] lines, int w, int h)
    {
        var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb);
        BitmapData d = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly,
                                    PixelFormat.Format24bppRgb);
        try
        {
            var row = new byte[Math.Abs(d.Stride)];
            for (int y = 0; y < h; y++)
            {
                int basePos = y * w * 2;
                for (int x = 0; x < w; x++)
                {
                    double yy = (lines[basePos + x * 2 + 1] - 16) / 219.0;
                    int pair = (x / 2) * 4;
                    double cb = lines[basePos + pair];
                    double cr = lines[basePos + pair + 2];
                    double r = yy + 0.701 * (cr - 128) / 112.0;
                    double blue = yy + 0.886 * (cb - 128) / 112.0;
                    double g = (yy - 0.299 * r - 0.114 * blue) / 0.587;
                    row[x * 3] = Clip(blue);
                    row[x * 3 + 1] = Clip(g);
                    row[x * 3 + 2] = Clip(r);
                }
                Marshal.Copy(row, 0, d.Scan0 + y * d.Stride, row.Length);
            }
        }
        finally { bmp.UnlockBits(d); }
        return bmp;
    }

    private static byte Clip(double v)
    {
        double s = v * 255.0;
        if (s < 0) s = 0;
        if (s > 255) s = 255;
        return (byte)Math.Round(s);
    }
}

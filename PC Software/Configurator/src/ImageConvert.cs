// Image conversion to 4:2:2 YCbCr as the firmware expects it. A second
// implementation of Firmware/tools/convert_image.py; compare.py checks
// the two byte for byte on the same input. Two details that must match
// exactly: Math.Round's default banker's rounding matches numpy's
// np.round and must not be overridden with MidpointRounding; the flicker
// filter runs on unrounded values, before chroma halving and limiting.
using System;

namespace PalSign;

public static class ImageConvert
{
    // Bump this on any change to the pixel arithmetic below (FitIn,
    // AntiTwitter, To422) and bump ALGO_VERSION in
    // Firmware/tools/convert_image.py to the same number in the same
    // commit. compare.py checks the two match before it trusts its own
    // byte-for-byte result.
    public const int AlgoVersion = 1;

    public const int ScreenW = 720;
    public const int ScreenH = 576;

    // On a 4:3 screen an image pixel is 720/576 divided by 4/3 times as
    // wide as it is high.
    public const double PixelAspect = (4.0 / 3.0) / ((double)ScreenW / ScreenH);

    /// <summary>Scale to 720x576 without mangling the aspect ratio.</summary>
    /// <param name="fill">crop until the picture fills, instead of black bars</param>
    /// <param name="square">convert a source that is already 720x576 anyway</param>
    public static double[,,] FitIn(double[,,] src, bool fill, bool square)
    {
        int h = src.GetLength(0), w = src.GetLength(1);

        // Already exactly the video raster: then it is not a picture with
        // square pixels but an image meant to be this way. Leave it
        // alone, otherwise bars wrongly appear left and right.
        if (w == ScreenW && h == ScreenH && !square) return src;

        // From screen ratio to raster ratio: a raster pixel is wider than
        // it is high, so fewer of them fit side by side.
        double srcRatio = (double)w / h / PixelAspect;
        double targetRatio = (double)ScreenW / ScreenH;
        int nw, nh;
        if ((srcRatio > targetRatio) != fill)
        {
            nw = ScreenW;
            nh = Math.Max(1, (int)Math.Round(ScreenW / srcRatio, MidpointRounding.AwayFromZero));
        }
        else
        {
            nh = ScreenH;
            nw = Math.Max(1, (int)Math.Round(ScreenH * srcRatio, MidpointRounding.AwayFromZero));
        }

        double[,,] scaled = Lanczos.Scale(src, nw, nh);

        // Stick it on a black canvas, centred.
        var canvas = new double[ScreenH, ScreenW, 3];
        int ox = (ScreenW - nw) / 2, oy = (ScreenH - nh) / 2;
        for (int y = 0; y < nh; y++)
        {
            int dy = y + oy;
            if (dy < 0 || dy >= ScreenH) continue;
            for (int x = 0; x < nw; x++)
            {
                int dx = x + ox;
                if (dx < 0 || dx >= ScreenW) continue;
                for (int k = 0; k < 3; k++) canvas[dy, dx, k] = scaled[y, x, k];
            }
        }
        return canvas;
    }

    /// <summary>Removes interlace twitter (a row-to-row sign change lands
    /// in alternate fields and flickers at 25 Hz) from a channel.
    ///
    /// Kernel [1,2,1]/4 is derived, not chosen: the field difference at
    /// row y is d[y] = I[y] - (I[y-1]+I[y+1])/2, which for an alternating
    /// signal is 2*I[y], so removing half of d cancels it exactly. Costs
    /// vertical sharpness, which is the same component. `strength` mixes
    /// linearly between filtered and unfiltered. Edge rows repeat, not
    /// zero, to avoid a dark hem.</summary>
    public static double[,] AntiTwitter(double[,] channel, double strength)
    {
        if (strength <= 0) return channel;
        int h = channel.GetLength(0), w = channel.GetLength(1);
        var outp = new double[h, w];
        for (int y = 0; y < h; y++)
        {
            int yPrev = y == 0 ? 0 : y - 1;
            int yNext = y == h - 1 ? h - 1 : y + 1;
            for (int x = 0; x < w; x++)
            {
                double smooth = 0.5 * channel[y, x] + 0.25 * channel[yPrev, x] + 0.25 * channel[yNext, x];
                outp[y, x] = channel[y, x] + strength * (smooth - channel[y, x]);
            }
        }
        return outp;
    }

    /// <summary>
    /// sRGB to 4:2:2 YCbCr at studio levels (Y 16..235, chroma 16..240).
    /// Delivers 576 lines of 1440 bytes, in the order Cb Y Cr Y.
    /// </summary>
    public static byte[] To422(double[,,] rgb, double flicker)
    {
        int h = rgb.GetLength(0), w = rgb.GetLength(1);
        var y = new double[h, w];
        var cb = new double[h, w];
        var cr = new double[h, w];

        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++)
            {
                double r = rgb[j, i, 0] / 255.0, g = rgb[j, i, 1] / 255.0, bl = rgb[j, i, 2] / 255.0;
                double yy = 0.299 * r + 0.587 * g + 0.114 * bl;
                cb[j, i] = 128 + 112 * (bl - yy) / 0.886;
                cr[j, i] = 128 + 112 * (r - yy) / 0.701;
                y[j, i] = 16 + 219 * yy;
            }

        // The twitter out BEFORE anything is rounded or limited, and on
        // all three channels: in 4:2:2 chroma is halved horizontally but
        // full vertically, so a colour edge can blink just as well as a
        // brightness edge.
        y = AntiTwitter(y, flicker);
        cb = AntiTwitter(cb, flicker);
        cr = AntiTwitter(cr, flicker);

        var outp = new byte[h * w * 2];
        for (int j = 0; j < h; j++)
        {
            int row = j * w * 2;
            for (int i = 0; i < w; i++) outp[row + i * 2 + 1] = Clip(y[j, i], 16, 235);
            // Halve the chroma by averaging PAIRS and not by throwing one
            // away, which saves ragged colour edges.
            for (int i = 0; i < w / 2; i++)
            {
                outp[row + i * 4] = Clip((cb[j, i * 2] + cb[j, i * 2 + 1]) / 2, 16, 240);
                outp[row + i * 4 + 2] = Clip((cr[j, i * 2] + cr[j, i * 2 + 1]) / 2, 16, 240);
            }
        }
        return outp;
    }

    private static byte Clip(double v, int low, int high)
    {
        double r = Math.Round(v);
        if (r < low) r = low;
        if (r > high) r = high;
        return (byte)r;
    }
}

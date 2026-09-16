// Lanczos scaling, rebuilt the way PIL does it (not System.Drawing's
// HighQualityBicubic, a different filter) so compare.py can check this
// against Firmware/tools/convert_image.py. Separable, horizontal then
// vertical, normalised weights, Lanczos-3. On shrinking the filter width
// scales with the shrink factor (`filterScale`), as PIL does, to avoid
// aliasing. Not byte exact with PIL.
using System;

namespace PalSign;

public static class Lanczos
{
    private const double A = 3.0;   // Lanczos-3, like PIL

    private static double Kern(double x)
    {
        x = Math.Abs(x);
        if (x < 1e-12) return 1.0;
        if (x >= A) return 0.0;
        double px = Math.PI * x;
        return A * Math.Sin(px) * Math.Sin(px / A) / (px * px);
    }

    /// <summary>Scales an [h,w,3] picture to (newW, newH).</summary>
    public static double[,,] Scale(double[,,] src, int newW, int newH)
    {
        double[,,] middle = OneDirection(src, newW, horizontal: true);
        return OneDirection(middle, newH, horizontal: false);
    }

    private static double[,,] OneDirection(double[,,] src, int fresh, bool horizontal)
    {
        int h = src.GetLength(0), w = src.GetLength(1);
        int old = horizontal ? w : h;
        if (old == fresh) return src;

        var outp = horizontal ? new double[h, fresh, 3] : new double[fresh, w, 3];

        double scale = (double)old / fresh;
        // On shrinking (scale > 1) the support stretches with it; on
        // enlarging it stays at 3.
        double filterScale = Math.Max(1.0, scale);
        double support = A * filterScale;

        for (int i = 0; i < fresh; i++)
        {
            double centre = (i + 0.5) * scale;
            int from = Math.Max(0, (int)Math.Floor(centre - support + 0.5));
            int to = Math.Min(old, (int)Math.Ceiling(centre + support - 0.5) + 1);
            if (to <= from) { from = Math.Min(Math.Max(0, (int)centre), old - 1); to = from + 1; }

            int n = to - from;
            var weight = new double[n];
            double sum = 0.0;
            for (int j = 0; j < n; j++)
            {
                double wj = Kern((from + j + 0.5 - centre) / filterScale);
                weight[j] = wj;
                sum += wj;
            }
            if (Math.Abs(sum) < 1e-12) { for (int j = 0; j < n; j++) weight[j] = 1.0 / n; }
            else { for (int j = 0; j < n; j++) weight[j] /= sum; }

            if (horizontal)
            {
                for (int y = 0; y < h; y++)
                    for (int k = 0; k < 3; k++)
                    {
                        double s = 0.0;
                        for (int j = 0; j < n; j++) s += weight[j] * src[y, from + j, k];
                        outp[y, i, k] = s;
                    }
            }
            else
            {
                for (int x = 0; x < w; x++)
                    for (int k = 0; k < 3; k++)
                    {
                        double s = 0.0;
                        for (int j = 0; j < n; j++) s += weight[j] * src[from + j, x, k];
                        outp[i, x, k] = s;
                    }
            }
        }
        return outp;
    }
}

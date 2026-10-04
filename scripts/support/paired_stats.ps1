if (-not ('WeavePairedStats' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
public static class WeavePairedStats {
    static double Middle(double[] x) {
        return (x[(x.Length - 1) / 2] + x[x.Length / 2]) / 2;
    }
    public static double[] Summary(double[] values) {
        var x = (double[])values.Clone();
        Array.Sort(x);
        double mean = 0, variance = 0;
        foreach (double value in x) mean += value / x.Length;
        foreach (double value in x) variance += (value - mean) * (value - mean);
        return new double[] { Middle(x), mean == 0 ? 0 : Math.Sqrt(variance / (x.Length - 1)) / mean };
    }
    // Resample entire paired blocks, never individual RTTs or the four windows.
    public static double[] Interval(double[] ratios) {
        const int count = 20000;
        var random = new Random(3601);
        var boot = new double[count];
        var sample = new double[ratios.Length];
        for (int i = 0; i < count; ++i) {
            for (int j = 0; j < sample.Length; ++j)
                sample[j] = ratios[random.Next(ratios.Length)];
            Array.Sort(sample);
            boot[i] = Middle(sample);
        }
        Array.Sort(boot);
        return new double[] { boot[(int)Math.Floor(.05 * (count - 1))],
                              boot[(int)Math.Ceiling(.95 * (count - 1))] };
    }
    public static double[] IndependentInterval(double[] before, double[] after) {
        const int count = 20000;
        var random = new Random(9203);
        var a = new double[before.Length];
        var b = new double[after.Length];
        var boot = new double[count];
        for (int i = 0; i < count; ++i) {
            for (int j = 0; j < a.Length; ++j) a[j] = before[random.Next(before.Length)];
            for (int j = 0; j < b.Length; ++j) b[j] = after[random.Next(after.Length)];
            Array.Sort(a); Array.Sort(b);
            boot[i] = Middle(b) / Middle(a);
        }
        Array.Sort(boot);
        return new double[] { boot[(int)Math.Floor(.05 * (count - 1))],
                              boot[(int)Math.Ceiling(.95 * (count - 1))] };
    }
}
'@
}

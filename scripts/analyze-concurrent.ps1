param([Parameter(Mandatory = $true)][string]$Directory)
$ErrorActionPreference = 'Stop'
function ConvertTo-JsonNumber([double]$Value) {
    if ([double]::IsNaN($Value) -or [double]::IsInfinity($Value)) { return $null }
    return $Value
}
if (-not ('WeaveConcurrentStatistics' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
public static class WeaveConcurrentStatistics {
    public static double Median(double[] values) {
        var sorted = (double[])values.Clone();
        Array.Sort(sorted);
        int n = sorted.Length;
        return n % 2 == 0 ? (sorted[n/2-1] + sorted[n/2]) / 2 : sorted[n/2];
    }
    public static double[] RatioInterval(double[] baseline, double[] candidate, int seed) {
        var random = new Random(seed);
        var ratios = new double[20000];
        var a = new double[baseline.Length];
        var b = new double[candidate.Length];
        for (int i = 0; i < ratios.Length; ++i) {
            for (int j = 0; j < a.Length; ++j) a[j] = baseline[random.Next(baseline.Length)];
            for (int j = 0; j < b.Length; ++j) b[j] = candidate[random.Next(candidate.Length)];
            ratios[i] = Median(b) / Median(a);
        }
        Array.Sort(ratios);
        return new[] { ratios[999], ratios[18999] };
    }
    public static double CV(double[] values) {
        double mean = 0, square = 0;
        foreach (double x in values) mean += x;
        mean /= values.Length;
        foreach (double x in values) square += (x-mean)*(x-mean);
        return Math.Sqrt(square/(values.Length-1))/mean;
    }
}
'@
}
$metrics = @('roundtrips_per_second', 'client_cycles_per_op', 'p99_us', 'client_cpu_us_per_op', 'client_cores', 'p50_us', 'p999_us', 'peer_cores')
$rows = [Collections.Generic.List[object]]::new()
$sampleCounts = @{}
foreach ($run in @('comparison', 'confirmation')) {
    $data = Get-Content (Join-Path $Directory ($run + '.json')) -Raw | ConvertFrom-Json
    if (@($data.benchmarks | Where-Object error_occurred).Count) { throw "Invalid run: $run" }
    $samples = @($data.benchmarks | Where-Object run_type -eq 'iteration')
    $sampleCounts[$run] = $samples.Count
    foreach ($scheduler in @('Affine', 'Stealing')) {
        $prefix = 'WeaveExplicitConcurrent' + $scheduler
        $asioPrefix = if ($scheduler -eq 'Affine') { 'AsioConcurrentAffine' } else { 'AsioConcurrentShared' }
        $names = @($samples.run_name | Where-Object { $_.StartsWith($prefix + '/') } | Select-Object -Unique)
        foreach ($name in $names) {
            $suffix = $name.Substring($prefix.Length)
            $a = @($samples | Where-Object run_name -eq $name)
            $b = @($samples | Where-Object run_name -eq ('WeaveConcurrent' + $scheduler + $suffix))
            $c = @($samples | Where-Object run_name -eq ($asioPrefix + $suffix))
            if ($a.Count -lt 7 -or $a.Count -ne $b.Count -or $a.Count -ne $c.Count) { throw "Unmatched samples: $name" }
            $row = [ordered]@{ run = $run; scheduler = $scheduler; workload = $suffix; repetitions = $a.Count; metrics = [ordered]@{} }
            $index = 0
            foreach ($metric in $metrics) {
                [double[]]$av = @($a | ForEach-Object { $_.$metric })
                [double[]]$bv = @($b | ForEach-Object { $_.$metric })
                [double[]]$cv = @($c | ForEach-Object { $_.$metric })
                $am = [WeaveConcurrentStatistics]::Median($av)
                $bm = [WeaveConcurrentStatistics]::Median($bv)
                $cm = [WeaveConcurrentStatistics]::Median($cv)
                $interval = if ($am -gt 0) { [WeaveConcurrentStatistics]::RatioInterval($av, $bv, (1729 + $index++)) } else { $null }
                $classification = 'diagnostic'
                if ($metric -eq 'roundtrips_per_second') {
                    $classification = if ($interval[0] -ge 0.95) { 'within_limit' } elseif ($interval[1] -lt 0.95) { 'regression' } else { 'inconclusive' }
                } elseif ($metric -in @('client_cycles_per_op', 'p99_us')) {
                    $limit = if ($metric -eq 'p99_us') { 1.10 } else { 1.05 }
                    $classification = if ($interval[1] -le $limit) { 'within_limit' } elseif ($interval[0] -gt $limit) { 'regression' } else { 'inconclusive' }
                }
                $row.metrics[$metric] = [ordered]@{
                    explicit_result = $am; weave = $bm; asio = $cm
                    change_pct = if ($am -gt 0) { 100 * ($bm / $am - 1) } else { $null }
                    ratio_ci90 = if ($null -ne $interval) { @((ConvertTo-JsonNumber $interval[0]), (ConvertTo-JsonNumber $interval[1])) } else { $null }
                    cv_pct = @((ConvertTo-JsonNumber (100 * [WeaveConcurrentStatistics]::CV($av))), (ConvertTo-JsonNumber (100 * [WeaveConcurrentStatistics]::CV($bv))), (ConvertTo-JsonNumber (100 * [WeaveConcurrentStatistics]::CV($cv))))
                    status = $classification
                }
            }
            $rows.Add($row)
        }
    }
}
$report = [ordered]@{
    method = 'Independent percentile bootstrap of median ratios, 20000 resamples, deterministic seeds, 90% intervals; repetitions are the unit of resampling, not individual RTTs'
    caution = 'Exploratory per-case intervals, not simultaneous family-wise guarantees; CPU cycles are not CPU seconds; tail percentiles are medians of per-repetition percentiles, not a pooled percentile'
    non_finite_numbers = 'A null interval endpoint or CV denotes an undefined or unbounded value, including CPU-time ratios with zero baseline samples'
    provenance = [ordered]@{
        analyzer_sha256 = (Get-FileHash $PSCommandPath -Algorithm SHA256).Hash
        comparison_sha256 = (Get-FileHash (Join-Path $Directory 'comparison.json') -Algorithm SHA256).Hash
        confirmation_sha256 = (Get-FileHash (Join-Path $Directory 'confirmation.json') -Algorithm SHA256).Hash
    }
    sample_counts = $sampleCounts
    rows = $rows
}
$report | ConvertTo-Json -Depth 9 | Set-Content (Join-Path $Directory 'analysis.json')
foreach ($row in $rows) {
    $t = $row.metrics.roundtrips_per_second
    $c = $row.metrics.client_cycles_per_op
    $l = $row.metrics.p99_us
    '{0} {1} {2}: throughput {3:+0.0;-0.0}% ({4}); cycles/op {5:+0.0;-0.0}% ({6}); p99 {7:+0.0;-0.0}% ({8})' -f `
        $row.run, $row.scheduler, $row.workload, $t.change_pct, $t.status, $c.change_pct, $c.status, $l.change_pct, $l.status
}

param(
    [Parameter(Mandatory = $true)][string]$Directory,
    [string]$BaselineDirectory,
    [switch]$NoExit
)
$ErrorActionPreference = 'Stop'
function Require([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}
function Bits([uint64]$Value) {
    $count = 0
    while ($Value) { $Value = $Value -band ($Value - 1); ++$count }
    return $count
}
function Finite($Value, [double]$Minimum, [string]$Name) {
    Require ($null -ne $Value -and [double]$Value -ge $Minimum -and
        ![double]::IsNaN($Value) -and ![double]::IsInfinity($Value)) "Invalid $Name."
}

$protocol = 'weave-task-policy-5m-v1'
$report = $null
try {
    . (Join-Path $PSScriptRoot 'support/paired_stats.ps1')
    $directoryPath = (Resolve-Path $Directory).Path
    $environment = Get-Content (Join-Path $directoryPath 'environment.json') -Raw | ConvertFrom-Json
    Require ($environment.profile -eq $protocol) 'Gate requires the paired five-minute CI profile.'
    Require ($environment.isolate_cpus -is [bool]) 'Missing CPU placement configuration.'
    Require ($environment.duration_ms -eq 250 -and $environment.repetitions -eq 7) 'CI gate requires 250 ms and 7 paired blocks.'
    Require ($environment.limits.throughput_regression_pct -eq 5 -and
        $environment.limits.cpu_cycles_per_op_regression_pct -eq 5 -and
        $environment.limits.p99_regression_pct -eq 10) 'Gate limits differ from the predeclared protocol.'
    $rawPath = Join-Path $directoryPath 'paired.json'
    $data = Get-Content $rawPath -Raw | ConvertFrom-Json
    Require ($data.context.protocol -eq $protocol -and $data.context.smoke -eq 'false' -and
        $data.context.injected_work -eq '0') 'Not an unmodified full paired run.'
    Require ($data.context.duration_ms -eq '250' -and $data.context.blocks -eq '7' -and
        $data.context.weave_warmup_windows -eq '4' -and $data.context.asio_warmup_windows -eq '2') 'Measurement/warmup plan mismatch.'
    Require (@($data.benchmarks | Where-Object error_occurred).Count -eq 0) 'Benchmark reported an error.'
    $clientMask = [uint64]$data.context.client_affinity_mask
    $peerMask = [uint64]$data.context.peer_affinity_mask
    if ($environment.isolate_cpus) {
        Require ((Bits $clientMask) -eq 8 -and (Bits $peerMask) -eq 4 -and ($clientMask -band $peerMask) -eq 0) 'Invalid CPU placement.'
    } else {
        Require ($clientMask -eq 0 -and $peerMask -eq 0) 'Unexpected CPU placement.'
    }
    $samples = @($data.benchmarks)
    Require ($samples.Count -eq 756) 'Incomplete or extra measurement windows.'
    $workloads = @('64/4/1024/0', '1024/1/1024/0', '1024/4/1024/0', '1024/8/1024/0', '256/4/65536/0', '1024/4/1024/512')
    $sequence = 0
    $roundtrips = 0.0
    $expected = @(foreach ($fixture in 0..11) {
        foreach ($block in 0..6) {
            foreach ($phaseOrder in 0..1) {
                $phase = ($block + $fixture + $phaseOrder) % 2
                foreach ($slot in 0..3) {
                    $label = [int]($slot -in @(1, 2)) -bxor (($block + $fixture + $phase) % 2)
                    @($fixture, $phase, $block, $slot, $label, [int]($phase -eq 1 -and $label -eq 1))
                }
            }
        }
        foreach ($block in 0..6) { @($fixture, 2, $block, 0, 2, 2) }
    })
    # Loop output is flattened by PowerShell; regroup the six numeric plan fields.
    foreach ($sample in $samples) {
        $plan = $expected[($sequence * 6)..($sequence * 6 + 5)]
        $keys = @('fixture_id', 'phase', 'block', 'slot', 'label', 'implementation')
        foreach ($i in 0..5) {
            Require ($null -ne $sample.($keys[$i]) -and $sample.($keys[$i]) -eq $plan[$i]) "Wrong $($keys[$i]) at window $sequence."
        }
        Require ($null -ne $sample.sequence -and $sample.sequence -eq $sequence) 'Missing, duplicated, or reordered window.'
        $fixture = [int]$sample.fixture_id
        $scheduler = if ($fixture -lt 6) { 'Affine' } else { 'Stealing' }
        $parts = $workloads[$fixture % 6].Split('/')
        $name = 'Paired{0}/connections:{1}/workers:{2}/bytes:{3}/cpu:{4}' -f $scheduler, $parts[0], $parts[1], $parts[2], $parts[3]
        Require ($sample.run_name -eq $name -and $sample.run_type -eq 'iteration' -and
            $sample.repetitions -eq 7 -and $sample.repetition_index -eq $sample.block) 'Workload or repetition mismatch.'
        foreach ($metric in @('roundtrips_per_second', 'client_cycles_per_op', 'peer_cycles_per_op', 'wall_seconds',
                'samples', 'min_connection_samples', 'max_connection_samples', 'p50_us', 'p95_us', 'p99_us', 'p999_us', 'max_us')) {
            Finite $sample.$metric ([double]::Epsilon) "$metric at window $sequence"
        }
        foreach ($metric in @('client_cpu_us_per_op', 'client_cores', 'peer_cores')) {
            Finite $sample.$metric 0 "$metric at window $sequence"
        }
        Require ($sample.samples -ge 1000 -and $sample.samples -eq [Math]::Floor($sample.samples) -and
            $sample.min_connection_samples -ge 1 -and $sample.min_connection_samples -le $sample.max_connection_samples) 'Insufficient or invalid progress.'
        Require ($sample.samples -ge [int]$parts[0] * $sample.min_connection_samples -and
            $sample.samples -le [int]$parts[0] * $sample.max_connection_samples) 'Connection accounting mismatch.'
        Require ($sample.wall_seconds -ge 0.25) 'Truncated measurement.'
        Require ($sample.p50_us -le $sample.p95_us -and $sample.p95_us -le $sample.p99_us -and
            $sample.p99_us -le $sample.p999_us -and $sample.p999_us -le $sample.max_us) 'Invalid percentile ordering.'
        Require ([Math]::Abs($sample.roundtrips_per_second / ($sample.samples / $sample.wall_seconds) - 1) -lt 1e-8) 'Throughput accounting mismatch.'
        $roundtrips += $sample.samples
        ++$sequence
    }

    $checks = @()
    $rows = @(foreach ($fixture in 0..11) {
        $case = @($samples | Where-Object fixture_id -eq $fixture)
        $scheduler = if ($fixture -lt 6) { 'affine' } else { 'stealing' }
        $metrics = [ordered]@{}
        foreach ($metric in @('roundtrips_per_second', 'client_cycles_per_op', 'p99_us', 'client_cpu_us_per_op')) {
            $values = [ordered]@{}
            foreach ($label in 0..2) {
                $phase = if ($label -eq 2) { 2 } else { 1 }
                $summary = [WeavePairedStats]::Summary([double[]]@($case | Where-Object { $_.phase -eq $phase -and $_.label -eq $label } | ForEach-Object { $_.$metric }))
                $values[@('explicit_result', 'weave', 'asio')[$label]] = @{ median = $summary[0]; cv = $summary[1] }
            }
            if ($metric -ne 'client_cpu_us_per_op') {
                foreach ($phase in 0..1) {
                    $ratios = @(foreach ($block in 0..6) {
                        $windows = @($case | Where-Object { $_.phase -eq $phase -and $_.block -eq $block })
                        $logRatio = 0.0
                        foreach ($window in $windows) {
                            $sign = if ($window.label -eq 1) { 1 } else { -1 }
                            $logRatio += $sign * [Math]::Log($window.$metric) / 2
                        }
                        [Math]::Exp($logRatio)
                    })
                    $summary = [WeavePairedStats]::Summary($ratios)
                    $interval = [WeavePairedStats]::Interval($ratios)
                    $limit = if ($metric -eq 'p99_us') { 0.10 } else { 0.05 }
                    if ($phase -eq 0) {
                        $status = if ($interval[0] -ge 1 - $limit -and $interval[1] -le 1 + $limit) { 'pass' } else { 'unreliable' }
                    } elseif ($metric -eq 'roundtrips_per_second') {
                        $status = if ($interval[0] -ge 0.95) { 'pass' } elseif ($interval[1] -lt 0.95) { 'regression' } else { 'inconclusive' }
                    } else {
                        $status = if ($interval[1] -le 1 + $limit) { 'pass' } elseif ($interval[0] -gt 1 + $limit) { 'regression' } else { 'inconclusive' }
                    }
                    $kind = if ($phase -eq 0) { 'AA' } else { 'AB' }
                    $values[$kind] = [ordered]@{ ratio = $summary[0]; ratio_ci90 = $interval; block_cv = $summary[1]; block_ratios = $ratios; status = $status }
                    $checks += [ordered]@{ kind = $kind; scheduler = $scheduler; workload = $workloads[$fixture % 6]; metric = $metric
                        change_pct = ($summary[0] - 1) * 100; ratio_ci90 = $interval; status = $status }
                }
            }
            $metrics[$metric] = $values
        }
        [ordered]@{ scheduler = $scheduler; workload = $workloads[$fixture % 6]; metrics = $metrics }
    })
    $controls = @($checks | Where-Object kind -eq 'AA')
    $candidates = @($checks | Where-Object kind -eq 'AB')
    $unreliable = @($controls | Where-Object status -ne 'pass').Count
    $regressions = @($candidates | Where-Object status -eq 'regression').Count
    $inconclusive = @($candidates | Where-Object status -eq 'inconclusive').Count
    $decision = if ($unreliable) { 'measurement_unreliable' } elseif ($regressions) { 'regression' } elseif ($inconclusive) { 'inconclusive' } else { 'pass' }
    $historical = @()
    $baselineProvenance = $null
    if ($BaselineDirectory) {
        $baselinePath = Join-Path (Resolve-Path $BaselineDirectory).Path 'paired.json'
        $baselineEnvironmentPath = Join-Path $BaselineDirectory 'environment.json'
        $baseline = Get-Content $baselinePath -Raw | ConvertFrom-Json
        $baselineEnvironment = Get-Content $baselineEnvironmentPath -Raw | ConvertFrom-Json
        Require ($baseline.context.protocol -in @($protocol, 'weave2-paired-5m-v2')) 'Unsupported historical measurement protocol.'
        Require ($baseline.context.duration_ms -eq '250' -and $baseline.context.blocks -eq '7' -and
            $baseline.context.smoke -eq 'false' -and $baseline.context.injected_work -eq '0' -and
            $baseline.context.library_build_type -eq 'release' -and $data.context.library_build_type -eq 'release') 'Baseline must be an unmodified full Release measurement.'
        Require ($baseline.context.client_affinity_mask -eq $data.context.client_affinity_mask -and
            $baseline.context.peer_affinity_mask -eq $data.context.peer_affinity_mask) 'Historical CPU placement differs.'
        Require (($baselineEnvironment.cpu | ConvertTo-Json -Compress) -eq ($environment.cpu | ConvertTo-Json -Compress) -and
            $baselineEnvironment.os.Version -eq $environment.os.Version -and
            $baselineEnvironment.os.BuildNumber -eq $environment.os.BuildNumber -and
            $baselineEnvironment.power_scheme -eq $environment.power_scheme) 'Historical hardware, OS, or power policy differs.'
        Require ($baseline.benchmarks.Count -eq 756 -and @($baseline.benchmarks | Where-Object error_occurred).Count -eq 0) 'Incomplete or failed historical run.'
        foreach ($i in 0..755) {
            $prior = $baseline.benchmarks[$i]
            $now = $samples[$i]
            foreach ($key in @('fixture_id', 'phase', 'block', 'slot', 'label', 'implementation', 'sequence', 'run_name', 'run_type', 'repetitions', 'repetition_index')) {
                Require ($null -ne $prior.$key -and $prior.$key -eq $now.$key) "Historical plan mismatch at window $i."
            }
            foreach ($metric in @('roundtrips_per_second', 'client_cycles_per_op', 'p99_us', 'samples', 'wall_seconds', 'min_connection_samples')) {
                Finite $prior.$metric ([double]::Epsilon) 'historical counter'
            }
            Require ($prior.samples -ge 1000 -and $prior.min_connection_samples -ge 1 -and $prior.wall_seconds -ge 0.25 -and
                [Math]::Abs($prior.roundtrips_per_second / ($prior.samples / $prior.wall_seconds) - 1) -lt 1e-8) 'Invalid historical progress or throughput accounting.'
        }
        $baselineProvenance = @{ raw_sha256 = (Get-FileHash $baselinePath -Algorithm SHA256).Hash
            environment_sha256 = (Get-FileHash $baselineEnvironmentPath -Algorithm SHA256).Hash
            protocol = $baseline.context.protocol }
        $historical = @(foreach ($fixture in 0..11) {
            foreach ($metric in @('roundtrips_per_second', 'client_cycles_per_op', 'p99_us')) {
                $controlRatios = @(foreach ($block in 0..6) {
                    $log = 0.0
                    foreach ($window in @($baseline.benchmarks | Where-Object { $_.fixture_id -eq $fixture -and $_.phase -eq 0 -and $_.block -eq $block })) {
                        $sign = if ($window.label -eq 1) { 1 } else { -1 }
                        $log += $sign * [Math]::Log($window.$metric) / 2
                    }
                    [Math]::Exp($log)
                })
                $controlInterval = [WeavePairedStats]::Interval($controlRatios)
                $tolerance = if ($metric -eq 'p99_us') { 0.10 } else { 0.05 }
                Require ($controlInterval[0] -ge 1 - $tolerance -and $controlInterval[1] -le 1 + $tolerance) 'Historical same-code controls failed.'
                $groups = @(foreach ($set in @($baseline.benchmarks, $samples)) {
                    $values = @(foreach ($block in 0..6) {
                        $windows = @($set | Where-Object { $_.fixture_id -eq $fixture -and $_.phase -eq 1 -and $_.label -eq 1 -and $_.block -eq $block })
                        Require ($windows.Count -eq 2) 'Missing or duplicated historical Task windows.'
                        $log = 0.0
                        foreach ($window in $windows) {
                            Finite $window.$metric ([double]::Epsilon) 'historical metric'
                            Require ($window.implementation -eq 1 -and $window.wall_seconds -ge 0.25 -and
                                $window.samples -ge 1000 -and $window.min_connection_samples -ge 1) 'Invalid historical Task window.'
                            $log += [Math]::Log($window.$metric) / 2
                        }
                        [Math]::Exp($log)
                    })
                    ,([double[]]$values)
                })
                $before = [WeavePairedStats]::Summary($groups[0])[0]
                $after = [WeavePairedStats]::Summary($groups[1])[0]
                $interval = [WeavePairedStats]::IndependentInterval($groups[0], $groups[1])
                if ($metric -eq 'roundtrips_per_second') {
                    $status = if ($interval[0] -ge 0.95) { 'pass' } elseif ($interval[1] -lt 0.95) { 'regression' } else { 'inconclusive' }
                } else {
                    $limit = if ($metric -eq 'p99_us') { 1.10 } else { 1.05 }
                    $status = if ($interval[1] -le $limit) { 'pass' } elseif ($interval[0] -gt $limit) { 'regression' } else { 'inconclusive' }
                }
                [ordered]@{ scheduler = $(if ($fixture -lt 6) { 'affine' } else { 'stealing' }); workload = $workloads[$fixture % 6]
                    metric = $metric; before = $before; after = $after; change_pct = ($after / $before - 1) * 100
                    ratio_ci90 = $interval; status = $status }
            }
        })
        if (!$unreliable) {
            if (@($historical | Where-Object status -eq 'regression').Count) { $decision = 'regression' }
            elseif ($decision -eq 'pass' -and @($historical | Where-Object status -eq 'inconclusive').Count) { $decision = 'inconclusive' }
        }
    }
    $report = [ordered]@{
        protocol = $protocol; decision = $decision
        comparison = if ($BaselineDirectory) { 'Historical Task artifact plus in-process propagation-policy controls' } else { 'Propagation policy only; not a version-to-version non-regression decision' }
        historical_confidence = 'Independent 90% bootstrap intervals over seven block means per version; cross-run drift is not cancelled; not paired across binaries'
        historical_checks = $historical; baseline_provenance = $baselineProvenance
        scope = 'Windows IOCP; warmed persistent connections; fixed-concurrency closed-loop; not CPU-time or production-SLO certification'
        limits = @{ throughput_min_ratio = 0.95; client_cycles_max_ratio = 1.05; p99_max_ratio = 1.10 }
        confidence = '90% percentile bootstrap of median paired block ratios; 20000 whole-block resamples; 7 ABBA/BAAB blocks per phase/case; exploratory per-case coverage'
        controls_passed = 36 - $unreliable; controls_unreliable = $unreliable
        checks_passed = 36 - $regressions - $inconclusive; checks_regressed = $regressions; checks_inconclusive = $inconclusive
        trials = $samples.Count; verified_roundtrips = $roundtrips
        provenance = [ordered]@{
            checker_sha256 = (Get-FileHash $PSCommandPath -Algorithm SHA256).Hash
            stats_sha256 = (Get-FileHash (Join-Path $PSScriptRoot 'support/paired_stats.ps1') -Algorithm SHA256).Hash
            environment_sha256 = (Get-FileHash (Join-Path $directoryPath 'environment.json') -Algorithm SHA256).Hash
            raw_sha256 = (Get-FileHash $rawPath -Algorithm SHA256).Hash
        }
        checks = $checks; rows = $rows
    }
} catch {
    $report = [ordered]@{ protocol = $protocol; decision = 'invalid'; error = $_.Exception.Message }
}
if (Test-Path $Directory -PathType Container) {
    $report | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $Directory 'gate.json')
}
Write-Host "Task gate: $($report.decision)"
if ($report.error) { Write-Host $report.error }
else { Write-Host "$($report.controls_passed)/36 A/A controls; $($report.checks_passed)/36 A/B checks passed; $($report.checks_regressed) regressed; $($report.checks_inconclusive) inconclusive." }
if ($report.historical_checks.Count) {
    Write-Host "$(@($report.historical_checks | Where-Object status -eq pass).Count)/36 historical checks passed."
}
if ($NoExit) { return $report }
switch ($report.decision) {
    'pass' { exit 0 }
    'regression' { exit 1 }
    'inconclusive' { exit 2 }
    'measurement_unreliable' { exit 5 }
    default { exit 3 }
}

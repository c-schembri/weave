$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('weave-gate-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $root | Out-Null
$checker = Join-Path $PSScriptRoot '../scripts/check-concurrent-gate.ps1'
$environment = @{
    profile = 'weave-task-policy-5m-v1'; isolate_cpus = $true; duration_ms = 250; repetitions = 7
    cpu = @(@{ Name = 'Fixture CPU'; NumberOfCores = 12; NumberOfLogicalProcessors = 24 })
    os = @{ Version = '10.0'; BuildNumber = 'fixture' }; power_scheme = 'fixture power'
    limits = @{ throughput_regression_pct = 5; cpu_cycles_per_op_regression_pct = 5; p99_regression_pct = 10 }
}
function New-Samples([string]$Mode) {
    $sequence = 0
    $workloads = @('64/4/1024/0', '1024/1/1024/0', '1024/4/1024/0', '1024/8/1024/0', '256/4/65536/0', '1024/4/1024/512')
    foreach ($fixture in 0..11) {
        $parts = $workloads[$fixture % 6].Split('/')
        $scheduler = if ($fixture -lt 6) { 'Affine' } else { 'Stealing' }
        $name = 'Paired{0}/connections:{1}/workers:{2}/bytes:{3}/cpu:{4}' -f $scheduler, $parts[0], $parts[1], $parts[2], $parts[3]
        $plan = @(foreach ($block in 0..6) {
            foreach ($order in 0..1) {
                $phase = ($block + $fixture + $order) % 2
                foreach ($slot in 0..3) {
                    $label = [int]($slot -in @(1, 2)) -bxor (($block + $fixture + $phase) % 2)
                    [pscustomobject]@{ phase = $phase; block = $block; slot = $slot; label = $label }
                }
            }
        }
        foreach ($block in 0..6) { [pscustomobject]@{ phase = 2; block = $block; slot = 0; label = 2 } })
        foreach ($p in $plan) {
            $scale = if ($Mode -eq 'drift') { [Math]::Exp(0.08 * $p.slot + 0.03 * $p.block) } else { 1 }
            $rate = 100000.0 * $scale
            $cycles = 1000.0 * $scale
            if ($Mode -eq 'historical_cycles') { $cycles *= 1.20 }
            if ($Mode -eq 'historical_noise') { $cycles *= $(if ($p.block % 2) { 1.10 } else { 0.99 }) }
            $tail = 100.0 * $scale
            if ($p.phase -eq 1 -and $p.label -eq 1) {
                switch ($Mode) {
                    'throughput' { $rate *= 0.9 }
                    'cycles' { $cycles *= 1.10 }
                    'tail' { $tail *= 1.20 }
                    'noisy' { $rate *= $(if ($p.block % 2) { 0.90 } else { 1.02 }) }
                }
            }
            if ($p.phase -eq 0 -and $p.label -eq 1) {
                if ($Mode -eq 'aa_bias') { $rate *= 0.8 }
                if ($Mode -eq 'aa_improves') { $rate *= 1.2 }
            }
            $count = [Math]::Round($rate * 0.25)
            [pscustomobject]@{
                run_name = $name; run_type = 'iteration'; repetitions = 7; repetition_index = $p.block
                fixture_id = $fixture; phase = $p.phase; block = $p.block; slot = $p.slot; label = $p.label
                implementation = $(if ($p.phase -eq 2) { 2 } else { [int]($p.phase -eq 1 -and $p.label -eq 1) })
                sequence = $sequence++; samples = $count; min_connection_samples = 1; max_connection_samples = $count
                wall_seconds = 0.25; roundtrips_per_second = $count / 0.25; client_cycles_per_op = $cycles
                peer_cycles_per_op = 1000; client_cpu_us_per_op = 10; client_cores = 1; peer_cores = 1
                p50_us = 25; p95_us = 50; p99_us = $tail; p999_us = $tail * 2; max_us = $tail * 3
            }
        }
    }
}
$script:tests = 0
function Check-Decision([string]$Name, [string]$Expected, [object[]]$Samples, [string]$ClientMask = '255', [string]$PeerMask = '3840', [switch]$Historical) {
    $environment | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $root 'environment.json')
    @{
        context = @{ protocol = 'weave-task-policy-5m-v1'; smoke = 'false'; injected_work = '0'; blocks = '7'
            library_build_type = 'release'
            duration_ms = '250'; weave_warmup_windows = '4'; asio_warmup_windows = '2'
            client_affinity_mask = $ClientMask; peer_affinity_mask = $PeerMask }
        benchmarks = $Samples
    } | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $root 'paired.json')
    $arguments = @{ Directory = $root; NoExit = $true }
    if ($Historical) { $arguments.BaselineDirectory = Join-Path $root 'baseline' }
    $result = & $checker @arguments
    if ($result.decision -ne $Expected) { throw "$Name expected $Expected, got $($result.decision): $($result.error)" }
    if ($Expected -ne 'invalid' -and ($result.checks.Count -ne 72 -or $result.rows.Count -ne 12)) { throw 'Incomplete analysis.' }
    ++$script:tests
    Write-Host "PASS: $Name"
}
try {
    $baseline = @(New-Samples 'pass')
    Check-Decision 'identical inputs pass' 'pass' $baseline
    $historicalRoot = Join-Path $root 'baseline'
    New-Item -ItemType Directory $historicalRoot | Out-Null
    Copy-Item (Join-Path $root 'environment.json'), (Join-Path $root 'paired.json') -Destination $historicalRoot
    Check-Decision 'identical historical artifact passes' 'pass' $baseline -Historical
    Check-Decision 'shared-core regression is not hidden by passing A/B controls' 'regression' @(New-Samples 'historical_cycles') -Historical
    Check-Decision 'historical uncertainty is not pass' 'inconclusive' @(New-Samples 'historical_noise') -Historical
    $baselineEnvironment = Get-Content (Join-Path $historicalRoot 'environment.json') -Raw | ConvertFrom-Json
    $baselineEnvironment.cpu[0].Name = 'Different CPU'
    $baselineEnvironment | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $historicalRoot 'environment.json')
    Check-Decision 'historical hardware mismatch rejected' 'invalid' $baseline -Historical
    Check-Decision 'balanced blocks cancel log-linear drift' 'pass' @(New-Samples 'drift')
    foreach ($metric in @('throughput', 'cycles', 'tail')) {
        Check-Decision "$metric regression detected" 'regression' @(New-Samples $metric)
    }
    Check-Decision 'noisy comparison cannot pass' 'inconclusive' @(New-Samples 'noisy')
    Check-Decision 'A/A bias invalidates measurement' 'measurement_unreliable' @(New-Samples 'aa_bias')
    Check-Decision 'A/A false improvement also invalidates measurement' 'measurement_unreliable' @(New-Samples 'aa_improves')
    Check-Decision 'partial matrix rejected' 'invalid' $baseline[1..755]
    $invalid = @(New-Samples 'pass')
    $invalid[1].sequence = 0
    Check-Decision 'duplicate sequence rejected' 'invalid' $invalid
    $invalid = @(New-Samples 'pass')
    $invalid[0].implementation = 1
    Check-Decision 'A/A must execute identical implementation' 'invalid' $invalid
    $invalid = @(New-Samples 'pass')
    $invalid[0].PSObject.Properties.Remove('client_cycles_per_op')
    Check-Decision 'missing CPU counter rejected' 'invalid' $invalid
    $invalid = @(New-Samples 'pass')
    $invalid[0].p99_us = 0
    Check-Decision 'zero percentile rejected' 'invalid' $invalid
    $invalid[0].p99_us = 100
    $invalid[0].min_connection_samples = 0
    Check-Decision 'stalled connection rejected' 'invalid' $invalid
    $invalid = @(New-Samples 'pass')
    $invalid[0] | Add-Member error_occurred $true
    Check-Decision 'benchmark error rejected' 'invalid' $invalid
    $environment.isolate_cpus = $false
    Check-Decision 'unrequested affinity rejected' 'invalid' $baseline
    Check-Decision 'OS-default placement supported' 'pass' $baseline '0' '0'
    Write-Host "$script:tests gate tests passed."
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName($resolved) -ne $temp -or [IO.Path]::GetFileName($resolved) -notlike 'weave-gate-*') {
        throw 'Refusing cleanup outside the test temporary directory.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

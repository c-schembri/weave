param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../build/windows'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('../benchmarks/results/ci-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [switch]$IsolateCpus,
    [string]$BaselineDirectory,
    [ValidateRange(5, 300)][int]$TimeoutSeconds = 300,
    [switch]$Worker
)
$ErrorActionPreference = 'Stop'
if ($Worker) {
    if ($BaselineDirectory) {
        $baseline = Get-Content (Join-Path $BaselineDirectory 'environment.json') -Raw | ConvertFrom-Json
        if ($baseline.isolate_cpus -ne [bool]$IsolateCpus) { throw 'Match the baseline CPU placement before measuring.' }
    }
    & (Join-Path $PSScriptRoot 'bench-concurrent.ps1') -BuildDirectory $BuildDirectory `
        -OutputDirectory $OutputDirectory -Profile CI -IsolateCpus:$IsolateCpus
    & (Join-Path $PSScriptRoot 'check-concurrent-gate.ps1') -Directory $OutputDirectory -BaselineDirectory $BaselineDirectory
    exit $LASTEXITCODE
}

$clock = [Diagnostics.Stopwatch]::StartNew()
if (Test-Path $OutputDirectory) { throw 'Use a new evidence directory; refusing to overwrite an existing run.' }
New-Item -ItemType Directory $OutputDirectory | Out-Null
$output = (Resolve-Path $OutputDirectory).Path
$exitCode = 3
$failure = $null
try {
    $build = (Resolve-Path $BuildDirectory).Path
    . (Join-Path $PSScriptRoot 'support/gate_process.ps1')
    $shell = (Get-Process -Id $PID).Path
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
        '-Worker', '-BuildDirectory', $build, '-OutputDirectory', $output)
    if ($IsolateCpus) { $arguments += '-IsolateCpus' }
    if ($BaselineDirectory) { $arguments += @('-BaselineDirectory', (Resolve-Path $BaselineDirectory).Path) }
    Write-Host "CI gate: full matrix, 7 paired A/A and A/B blocks at 250 ms/window; ${TimeoutSeconds}s total budget."
    # Reserve time for process-tree cleanup and evidence publication.
    $remaining = [int]($TimeoutSeconds * 1000 - $clock.ElapsedMilliseconds - 3000)
    $exitCode = [WeaveGateProcess]::Run($shell, $arguments, (Split-Path $PSScriptRoot -Parent),
        (Join-Path $output 'run.log'), $remaining)
    if ($exitCode -eq -1) { $exitCode = 4; $failure = 'Wall-clock budget exhausted; worker and all descendants terminated.' }
    elseif ($exitCode -notin @(0, 1, 2, 3, 5)) { $failure = "Worker failed with exit code $exitCode"; $exitCode = 3 }
} catch {
    $failure = $_.Exception.Message
}
if ($failure -or !(Test-Path (Join-Path $output 'gate.json'))) {
    if (!$failure) { $failure = 'Worker did not produce a gate decision.'; $exitCode = 3 }
    @{
        protocol = 'weave-task-policy-5m-v1'
        decision = if ($exitCode -eq 4) { 'timeout' } else { 'invalid' }
        error = $failure
    } | ConvertTo-Json | Set-Content (Join-Path $output 'gate.json')
}
$decision = Get-Content (Join-Path $output 'gate.json') -Raw | ConvertFrom-Json
$run = [ordered]@{
    protocol = 'weave-task-policy-5m-v1'; decision = $decision.decision; exit_code = $exitCode
    budget_seconds = $TimeoutSeconds; elapsed_seconds = $clock.Elapsed.TotalSeconds
    runner_sha256 = (Get-FileHash $PSCommandPath -Algorithm SHA256).Hash
    supervisor_sha256 = (Get-FileHash (Join-Path $PSScriptRoot 'support/gate_process.ps1') -Algorithm SHA256).Hash
}
$run | ConvertTo-Json | Set-Content (Join-Path $output 'run.json')
Write-Host ("Gate: {0}; elapsed {1:N1}s; exit {2}." -f $decision.decision, $run.elapsed_seconds, $exitCode)
if ($decision.error) { Write-Host $decision.error }
else { Write-Host "$($decision.controls_passed)/36 A/A controls; $($decision.checks_passed)/36 A/B checks passed; $($decision.checks_regressed) regressed; $($decision.checks_inconclusive) inconclusive." }
if ($decision.historical_checks.Count) {
    Write-Host "$(@($decision.historical_checks | Where-Object status -eq pass).Count)/36 historical checks passed."
}
Write-Host "Evidence: $output"
exit $exitCode

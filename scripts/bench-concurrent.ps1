param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../build/windows'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('../benchmarks/results/concurrent-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [int]$DurationMilliseconds = 1000,
    [int]$Repetitions = 7,
    [string]$Filter = '.',
    [switch]$IsolateCpus,
    [ValidateSet('Full', 'CI')][string]$Profile = 'Full'
)
$ErrorActionPreference = 'Stop'
if ($Profile -eq 'CI') {
    if ($Filter -ne '.') { throw 'The CI gate requires the full workload matrix.' }
    $DurationMilliseconds = 250
    $Repetitions = 7
} elseif ($DurationMilliseconds -lt 1000 -or $DurationMilliseconds -gt 10000 -or $Repetitions -lt 7) {
    throw 'Performance evidence requires at least 1000 ms and 7 repetitions; use the executable directly for smoke tests.'
}
$binary = (Resolve-Path (Join-Path $BuildDirectory 'Release/weave_concurrent.exe')).Path
if ((Test-Path (Join-Path $OutputDirectory 'comparison.json')) -or (Test-Path (Join-Path $OutputDirectory 'paired.json'))) {
    throw 'Refusing to overwrite existing evidence.'
}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$repo = Split-Path $PSScriptRoot -Parent
$sourceFiles = @(
    Get-ChildItem (Join-Path $repo 'modules') -Recurse -File
    Get-ChildItem (Join-Path $repo 'test_support') -Recurse -File
    Get-ChildItem (Join-Path $repo 'cmake') -Recurse -File
    Get-ChildItem (Join-Path $repo 'benchmarks/support') -Recurse -File
    Get-ChildItem (Join-Path $repo 'benchmarks/integration') -Recurse -File
    Get-ChildItem (Join-Path $PSScriptRoot 'support') -Recurse -File
    Get-Item (Join-Path $repo 'benchmarks/CMakeLists.txt'), (Join-Path $repo 'CMakeLists.txt')
    Get-Item (Join-Path $PSScriptRoot 'bench-concurrent.ps1'), (Join-Path $PSScriptRoot 'analyze-concurrent.ps1')
    Get-Item (Join-Path $PSScriptRoot 'check-concurrent-gate.ps1')
    Get-Item (Join-Path $PSScriptRoot 'run-concurrent-gate.ps1')
    Get-Item (Join-Path $repo 'docs/gate.md')
)
$metadata = [ordered]@{
    timestamp = (Get-Date).ToString('o')
    revision = (git -C $repo rev-parse HEAD)
    worktree = @(git -C $repo status --porcelain)
    cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
    os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber
    power_scheme = (& powercfg /GETACTIVESCHEME)
    configuration = 'Release; exceptions disabled; native Task IOCP; separate peer process'
    reference = 'Current Task with explicit as_result checks versus automatic propagation; not a historical release baseline'
    compiler = @(Get-ChildItem $BuildDirectory -Recurse -Filter CMakeCXXCompiler.cmake |
        Select-String 'CMAKE_CXX_COMPILER_VERSION ' | ForEach-Object { $_.Line })
    duration_ms = $DurationMilliseconds
    profile = if ($Profile -eq 'CI') { 'weave-task-policy-5m-v1' } else { 'full' }
    repetitions = $Repetitions
    filter = $Filter
    peer_workers = 4
    random_interleaving = $Profile -ne 'CI'
    pairing = if ($Profile -eq 'CI') { 'Same fixture; ABBA/BAAB; explicit/explicit controls and explicit/automatic comparison; all blocks retained' } else { $null }
    isolate_cpus = [bool]$IsolateCpus
    limits = @{ throughput_regression_pct = 5; cpu_cycles_per_op_regression_pct = 5; p99_regression_pct = 10 }
    cpu_accounting = 'GetProcessTimes CPU seconds/core equivalents and QueryProcessCycleTime cycles per operation; no conversion from cycles to elapsed time'
    latency = 'Closed-loop, every completed RTT recorded, p99 per repetition; no coordinated-omission correction or open-loop arrival model'
    executable = Get-FileHash $binary -Algorithm SHA256
    sources = @($sourceFiles | Get-FileHash -Algorithm SHA256)
}
foreach ($flag in @('WEAVE_ENABLE_ASAN', 'WEAVE_PROFILE_RUNTIME')) {
    if (Select-String -Path (Join-Path $BuildDirectory 'CMakeCache.txt') -Pattern "^${flag}:BOOL=ON$") {
        throw "Timing requires ${flag}=OFF."
    }
}
$metadata | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputDirectory 'environment.json')
[string[]]$placement = if ($IsolateCpus) { @('--weave_isolate_cpus') } else { @() }
if ($Profile -eq 'CI') {
    & $binary @placement "--weave_paired_out=$(Join-Path $OutputDirectory 'paired.json')"
    if ($LASTEXITCODE) { throw 'Paired benchmark failed.' }
    Write-Host "Evidence: $OutputDirectory"
    return
}
foreach ($run in @('comparison', 'confirmation')) {
    $output = Join-Path $OutputDirectory ($run + '.json')
    & $binary @placement "--weave_duration_ms=$DurationMilliseconds" "--benchmark_filter=$Filter" `
        "--benchmark_repetitions=$Repetitions" --benchmark_enable_random_interleaving=true `
        --benchmark_display_aggregates_only=true "--benchmark_out=$output" --benchmark_out_format=json
    if ($LASTEXITCODE) { throw "Benchmark failed: $run" }
    $data = Get-Content $output -Raw | ConvertFrom-Json
    if (@($data.benchmarks | Where-Object { $_.error_occurred }).Count) { throw "Invalid samples: $run" }
    $raw = @($data.benchmarks | Where-Object run_type -eq 'iteration')
    if (!$raw.Count -or @($raw | Where-Object { $_.samples -lt 1000 -or $_.min_connection_samples -lt 1 }).Count) {
        throw "Insufficient samples or stalled connection: $run"
    }
}
if ($Profile -ne 'CI') {
    & (Join-Path $PSScriptRoot 'analyze-concurrent.ps1') -Directory $OutputDirectory
}
Write-Host "Evidence: $OutputDirectory"

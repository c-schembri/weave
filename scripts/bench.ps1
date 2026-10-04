param(
    [double]$Seconds = 0.5,
    [int]$Repetitions = 5,
    [string]$Filter = '.'
)
$ErrorActionPreference = 'Stop'
if ($Seconds -le 0 -or $Repetitions -lt 1) { throw 'Invalid benchmark duration or repetitions.' }
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    cmake --preset windows
    if ($LASTEXITCODE) { throw 'Configure failed.' }
    cmake --build --preset release --parallel
    if ($LASTEXITCODE) { throw 'Build failed.' }
    ctest --preset release
    if ($LASTEXITCODE) { throw 'Tests failed.' }
    $directory = Join-Path 'out' (Get-Date -Format 'yyyyMMdd-HHmmss')
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $output = Join-Path $directory 'benchmark.json'
    $duration = $Seconds.ToString([Globalization.CultureInfo]::InvariantCulture) + 's'
    & ./build/windows/Release/weave_bench.exe "--benchmark_min_time=$duration" "--benchmark_repetitions=$Repetitions" "--benchmark_filter=$Filter" --benchmark_enable_random_interleaving=true "--benchmark_out=$output" --benchmark_out_format=json
    if ($LASTEXITCODE) { throw 'Benchmark process failed.' }
    $results = Get-Content $output -Raw | ConvertFrom-Json
    if (@($results.benchmarks | Where-Object { $_.error_occurred }).Count) { throw 'A benchmark reported an error.' }
    $metadata = [ordered]@{
        timestamp = (Get-Date).ToString('o')
        revision = (git rev-parse HEAD 2>$null)
        worktree = @(git status --porcelain)
        cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
        os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber
        compiler = 'MSVC x64; see build/windows/CMakeFiles/*/CMakeCXXCompiler.cmake for exact version'
        cmake = (cmake --version | Select-Object -First 1)
        seconds = $Seconds
        repetitions = $Repetitions
        filter = $Filter
        configuration = 'Release'
    }
    $metadata | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $directory 'environment.json')
    Write-Host "Results: $directory"
} finally {
    Pop-Location
}

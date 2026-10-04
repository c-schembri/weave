param([Parameter(Mandatory = $true)][string]$Binary)
$ErrorActionPreference = 'Stop'
$path = Join-Path ([IO.Path]::GetTempPath()) ('weave-paired-' + [Guid]::NewGuid().ToString('N') + '.json')
try {
    & $Binary --weave_paired_smoke --weave_paired_positive_control "--weave_paired_out=$path"
    if ($LASTEXITCODE) { throw 'Native positive control failed.' }
    $data = Get-Content $path -Raw | ConvertFrom-Json
    if ($data.context.injected_work -ne '200000' -or $data.benchmarks.Count -ne 36) { throw 'Incorrect smoke plan.' }
    $sequence = 0
    foreach ($sample in $data.benchmarks) {
        if ($sample.sequence -ne $sequence++ -or $sample.error_occurred -or $sample.min_connection_samples -lt 1) {
            throw 'Invalid or incomplete native sample.'
        }
    }
    . (Join-Path $PSScriptRoot '../scripts/support/paired_stats.ps1')
    foreach ($fixture in @(0, 6)) {
        $case = @($data.benchmarks | Where-Object { $_.fixture_id -eq $fixture -and $_.phase -eq 1 })
        $a = [WeavePairedStats]::Summary([double[]]@($case | Where-Object label -eq 0 | ForEach-Object client_cycles_per_op))
        $b = [WeavePairedStats]::Summary([double[]]@($case | Where-Object label -eq 1 | ForEach-Object client_cycles_per_op))
        if ($b[0] / $a[0] -lt 1.5) { throw 'Harness did not detect deliberately injected CPU cost.' }
        Write-Host ("Positive control fixture {0}: {1:N2}x client cycles/op" -f $fixture, ($b[0] / $a[0]))
    }
} finally {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
}

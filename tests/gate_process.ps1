$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/support/gate_process.ps1')
$root = Join-Path ([IO.Path]::GetTempPath()) ('weave-gate-process-' + [Guid]::NewGuid().ToString('N') + ' with spaces')
New-Item -ItemType Directory $root | Out-Null
$shell = (Get-Process -Id $PID).Path
$fixture = Join-Path $PSScriptRoot 'support/gate_process_fixture.ps1'
try {
    $code = [WeaveGateProcess]::Run($shell, @('-NoProfile', '-NonInteractive', '-File', $fixture, '-Mode', 'exit'),
        $root, (Join-Path $root 'exit.log'), 5000)
    if ($code -ne 7 -or (Get-Content (Join-Path $root 'exit.log') -Raw) -notmatch 'expected exit') {
        throw 'Exit code or redirected log was lost.'
    }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $code = [WeaveGateProcess]::Run($shell, @('-NoProfile', '-NonInteractive', '-File', $fixture, '-Mode', 'hang', '-Directory', $root),
        $root, (Join-Path $root 'timeout.log'), 2500)
    if ($code -ne -1 -or $clock.Elapsed.TotalSeconds -gt 5) { throw 'Deadline was not enforced.' }
    foreach ($name in @('parent', 'child')) {
        $processId = [int](Get-Content (Join-Path $root ($name + '.pid')))
        $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if ($process -and !$process.WaitForExit(1000)) { throw "Owned $name survived timeout." }
    }
    Write-Host 'Supervisor tests passed: exit status, logs, wall deadline, and descendant cleanup.'
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName($resolved) -ne $temp -or [IO.Path]::GetFileName($resolved) -notlike 'weave-gate-process-*') {
        throw 'Refusing cleanup outside the test temporary directory.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

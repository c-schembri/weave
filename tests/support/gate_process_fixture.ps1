param([string]$Mode, [string]$Directory)
$ErrorActionPreference = 'Stop'
if ($Mode -eq 'exit') { Write-Output 'expected exit'; exit 7 }
if ($Mode -eq 'child') {
    $PID | Set-Content (Join-Path $Directory 'child.pid')
    Start-Sleep -Seconds 60
    exit 0
}
$PID | Set-Content (Join-Path $Directory 'parent.pid')
$shell = (Get-Process -Id $PID).Path
Start-Process -FilePath $shell -WindowStyle Hidden -ArgumentList @('-NoProfile', '-NonInteractive',
    '-File', ('"' + $PSCommandPath + '"'), '-Mode', 'child', '-Directory', ('"' + $Directory + '"')) | Out-Null
Start-Sleep -Seconds 60

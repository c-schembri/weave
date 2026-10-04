param([Parameter(Mandatory = $true)][string]$Executable)
$ErrorActionPreference = 'Stop'

function Start-Echo([string]$Arguments) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Executable
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    if (-not $process.Start()) { throw 'Could not start example.' }
    return @{ Process = $process; Errors = $process.StandardError.ReadToEndAsync() }
}

function Stop-Echo($Server) {
    try {
        if (-not $Server.Process.HasExited) { $Server.Process.Kill() }
        if (-not $Server.Process.WaitForExit(5000)) { throw 'Example did not terminate.' }
        $errors = $Server.Errors.GetAwaiter().GetResult()
        if ($errors -match 'AddressSanitizer') { throw $errors }
    } finally { $Server.Process.Dispose() }
}

function Connect-Echo([int]$Port) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.NoDelay = $true
    $client.ReceiveTimeout = 5000
    $client.SendTimeout = 5000
    try {
        $connected = $client.ConnectAsync('127.0.0.1', $Port)
        if (-not $connected.Wait(5000)) { throw 'Connection timed out.' }
        $connected.GetAwaiter().GetResult() | Out-Null
        return $client
    } catch {
        $client.Dispose()
        throw
    }
}

function Read-Exactly($Stream, [int]$Count) {
    $buffer = New-Object byte[] $Count
    $offset = 0
    while ($offset -lt $Count) {
        $size = $Stream.Read($buffer, $offset, $Count - $offset)
        if ($size -eq 0) { throw "Early EOF at $offset of $Count bytes." }
        $offset += $size
    }
    return ,$buffer
}

function Check-Bytes([byte[]]$Expected, [byte[]]$Actual) {
    if ([Convert]::ToBase64String($Expected) -cne [Convert]::ToBase64String($Actual)) {
        throw 'Echo payload mismatch.'
    }
}

function Check-Eof($Client) {
    $stream = $Client.GetStream()
    $Client.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
    $byte = New-Object byte[] 1
    if ($stream.Read($byte, 0, 1) -ne 0) { throw 'Unexpected trailing echo data.' }
}

# All variants reject malformed ports and fail rather than silently sharing a listener.
foreach ($argument in @('-1', '65536', 'abc', '80junk')) {
    $bad = Start-Echo $argument
    try {
        if (-not $bad.Process.WaitForExit(5000) -or $bad.Process.ExitCode -ne 2) {
            throw "Invalid port '$argument' did not return usage error 2."
        }
    } finally { Stop-Echo $bad }
}
$reserved = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
$reserved.ExclusiveAddressUse = $true
$reserved.Start()
try {
    $bad = Start-Echo ([string]$reserved.LocalEndpoint.Port)
    try {
        if (-not $bad.Process.WaitForExit(5000) -or $bad.Process.ExitCode -ne 1) {
            throw 'Occupied port did not return startup error 1.'
        }
    } finally { Stop-Echo $bad }
} finally { $reserved.Stop() }

$server = Start-Echo '0'
$clients = New-Object 'System.Collections.Generic.List[System.Net.Sockets.TcpClient]'
try {
    $ready = $server.Process.StandardOutput.ReadLineAsync()
    if (-not $ready.Wait(10000)) { throw 'Server did not announce readiness.' }
    $line = $ready.GetAwaiter().GetResult()
    $match = [regex]::Match([string]$line, '^Listening on 127\.0\.0\.1:(\d+)$')
    if (-not $match.Success) { throw "Unexpected startup output: '$line'." }
    $port = [int]$match.Groups[1].Value

    # Leave the first connection idle. Other clients must still make progress.
    $clients.Add((Connect-Echo $port))
    for ($i = 0; $i -lt 4; ++$i) { $clients.Add((Connect-Echo $port)) }
    $payload = New-Object byte[] 65537
    for ($i = 0; $i -lt $payload.Length; ++$i) { $payload[$i] = ($i * 17 + 3) % 256 }
    foreach ($size in @(1, 31, 4096, 8193, 65537)) {
        $expected = New-Object byte[] $size
        [Array]::Copy($payload, $expected, $size)
        for ($i = 1; $i -lt $clients.Count; ++$i) {
            $stream = $clients[$i].GetStream()
            $stream.Write($expected, 0, 1)
            if ($size -gt 1) { $stream.Write($expected, 1, $size - 1) }
        }
        for ($i = 1; $i -lt $clients.Count; ++$i) {
            Check-Bytes $expected (Read-Exactly $clients[$i].GetStream() $size)
        }
    }
    foreach ($client in $clients) { Check-Eof $client }

    # A slow reader forces writes to queue. Half-close as soon as sending finishes,
    # even if echoed bytes are still pending, and require every byte before EOF.
    $slow = Connect-Echo $port
    $clients.Add($slow)
    $slow.ReceiveBufferSize = 65536
    $slow.SendBufferSize = 4096
    $large = New-Object byte[] (4 * 1024 * 1024)
    for ($offset = 0; $offset -lt $large.Length; $offset += $payload.Length) {
        [Array]::Copy($payload, 0, $large, $offset, [Math]::Min($payload.Length, $large.Length - $offset))
    }
    $stream = $slow.GetStream()
    $sending = $stream.WriteAsync($large, 0, $large.Length)
    Start-Sleep -Milliseconds 150
    $received = New-Object byte[] $large.Length
    $offset = 0
    $halfClosed = $false
    while ($offset -lt $received.Length) {
        if ($sending.IsCompleted -and -not $halfClosed) {
            $sending.GetAwaiter().GetResult() | Out-Null
            $slow.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
            $halfClosed = $true
        }
        $size = $stream.Read($received, $offset, [Math]::Min(16384, $received.Length - $offset))
        if ($size -eq 0) { throw 'Early EOF with backpressured echo data outstanding.' }
        $offset += $size
    }
    if (-not $sending.Wait(5000)) { throw 'Large send did not finish.' }
    $sending.GetAwaiter().GetResult() | Out-Null
    Check-Bytes $large $received
    if (-not $halfClosed) { $slow.Client.Shutdown([Net.Sockets.SocketShutdown]::Send) }
    if ($stream.Read((New-Object byte[] 1), 0, 1) -ne 0) { throw 'Trailing bulk echo data.' }

    # A reset must only terminate that connection, not the listener or other sessions.
    $reset = Connect-Echo $port
    $clients.Add($reset)
    $reset.Client.LingerState = New-Object Net.Sockets.LingerOption($true, 0)
    $reset.GetStream().Write($payload, 0, 4096)
    $reset.Dispose()
    $last = Connect-Echo $port
    $clients.Add($last)
    $last.GetStream().Write($payload, 0, $payload.Length)
    Check-Bytes $payload (Read-Exactly $last.GetStream() $payload.Length)
    Check-Eof $last
    if ($server.Process.HasExited) { throw 'Server exited unexpectedly.' }
    Write-Host 'PASS: concurrent clients, binary/fragmented data, backpressure, half-close, reset recovery, startup errors.'
} finally {
    foreach ($client in $clients) { $client.Dispose() }
    Stop-Echo $server
    if ($server.Errors.IsCompleted) {
        $errors = $server.Errors.GetAwaiter().GetResult()
        if ($errors) { Write-Host $errors }
    }
}

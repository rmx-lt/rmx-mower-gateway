# Send a command to the gateway and print its ack (wiki Mower-Protocol §5, §6.4).
#
#   tools\mqtt_cmd.ps1 ping
#   tools\mqtt_cmd.ps1 restart
#   tools\mqtt_cmd.ps1 ota -Version 0.4.0 -Size 851234   # file = <version>.bin (tools\release.ps1 uploads it)
#   tools\mqtt_cmd.ps1 monitor_start -Session m-1 -Metrics usage.total.up,usage.total.down -Rate 2 -Hold 20
#   tools\mqtt_cmd.ps1 monitor_stop -Session m-1
#   tools\mqtt_cmd.ps1 monitor_start -Session m -Count 12 -Metrics system.cpu -Rate 1   # 12 streams m-1 .. m-12
#   tools\mqtt_cmd.ps1 ping -Id c-test1 -Repeat 1     # same id again: the gateway repeats the same ack
#
# Publishes {"id","cmd",...,"expires_at"} on mower/<device>/gateway/cmd as the MQTT login "app"
# (the backend role: write mower/#) and waits for the first answer on mower/<device>/gateway/cmd/ack.
# An ota is acked twice (accepted, then done or failed after the restart): use tools\mqtt_watch.ps1
# to see the later acks. Needs the Mosquitto clients for Windows and `ssh rmx-vps`.
# The app password is read from the server into memory (or from $env:RMX_APP_MQTT_PW)
# and never printed. On 8883 the client id is forced to "app": a running backend
# using that login would be disconnected while this runs.
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Cmd,
    [string]$Version,         # ota: the image version (must match its header)
    [string]$File,            # ota: file name on the server (default <Version>.bin)
    [long]$Size = 0,          # ota: image size in bytes (0 = not checked)
    [string]$Session,         # monitor_start / monitor_stop: the stream's session id
    [string[]]$Metrics,       # monitor_start: metric names
    [int]$Rate = 0,           # monitor_start: samples per second
    [int]$Hold = 0,           # monitor_start: seconds without extension (0 = the gateway's default)
    [string]$Id,              # command id (default: a new one)
    [int]$Count = 0,          # monitor_start: start this many streams, <Session>-1 .. <Session>-N, one command each
    [int]$Repeat = 0,         # send the same command (same id) this many more times
    [int]$TtlS = 60,          # expires_at = now + TtlS (0 = no expiry, negative = already expired, for tests)
    [int]$TimeoutS = 45,
    [string]$Device = 'mower-01'
)
$ErrorActionPreference = 'Stop'
$rr = 'C:\Program Files\mosquitto\mosquitto_rr.exe'
$ca = Join-Path $PSScriptRoot '..\certs\rmx-ca-bundle.pem' | Resolve-Path

$pw = $env:RMX_APP_MQTT_PW
if (-not $pw) {
    # No double quotes in the remote command: Windows PowerShell 5.1 strips them from native args.
    # New SSH connections to the VPS are sometimes dropped: retry a few times.
    for ($try = 1; $try -le 3 -and -not $pw; $try++) {
        if ($try -gt 1) { Write-Host "ssh failed, retrying in 20 s ($try/3)"; Start-Sleep 20 }
        $pw = ssh -o BatchMode=yes -o ConnectTimeout=15 rmx-vps 'sudo awk ''/^app /{print $2}'' /opt/rmx-vps/services/mosquitto/data/credentials.txt'
    }
    if (-not $pw) { throw 'could not read the app password over ssh rmx-vps (3 tries)' }
}

if (-not $Id) { $Id = 'pc-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) }
$msg = [ordered]@{ id = $Id; cmd = $Cmd }
switch ($Cmd) {
    'ota' {
        if (-not $Version) { throw 'ota needs -Version' }
        $msg.version = $Version
        $msg.file = if ($File) { $File } else { "$Version.bin" }
        if ($Size -gt 0) { $msg.size = $Size }
    }
    'monitor_start' {
        if (-not $Session -or -not $Metrics -or $Rate -le 0) { throw 'monitor_start needs -Session, -Metrics and -Rate' }
        $msg.session = $Session
        $msg.metrics = @($Metrics | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # -File passes a,b as one string
        $msg.rate = $Rate
        if ($Hold -gt 0) { $msg.hold = $Hold }
    }
    'monitor_stop' {
        if ($Session) { $msg.session = $Session }
    }
}
if ($TtlS -ne 0) { $msg.expires_at = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + $TtlS }
$json = $msg | ConvertTo-Json -Compress -Depth 4

$last = 0
$sends = if ($Count -gt 1) { $Count - 1 } else { $Repeat }
for ($n = 0; $n -le $sends; $n++) {
    if ($Count -gt 1) {
        $Id = 'pc-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
        $msg.id = $Id
        $msg.session = "$Session-$($n + 1)"
        if ($TtlS -ne 0) { $msg.expires_at = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + $TtlS }
        $json = $msg | ConvertTo-Json -Compress -Depth 4
    }
    $out = New-TemporaryFile; $err = New-TemporaryFile
    # mosquitto_rr has no -f; pass the JSON with -m, quotes escaped for the Windows command line
    $a = @('-h', 'mqtt.rmx.lt', '-p', '8883', '--cafile', "`"$ca`"", '-V', '5', '-q', '1',
           '-u', 'app', '-P', $pw, '-t', "mower/$Device/gateway/cmd", '-e', "mower/$Device/gateway/cmd/ack",
           '-m', ('"' + ($json -replace '(\\*)"', '$1$1\"') + '"'), '-W', $TimeoutS)
    Write-Host "-> mower/$Device/gateway/cmd $json"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process $rr -ArgumentList $a -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow -PassThru
    # mosquitto's -W timeout is unreliable on Windows: enforce it here too
    if (-not $p.WaitForExit(($TimeoutS + 5) * 1000)) { $p.Kill(); $p.WaitForExit() }
    $reply = (Get-Content $out -Raw)
    $e = (Get-Content $err -Raw)
    Remove-Item $out, $err -ErrorAction SilentlyContinue

    if ($reply) {
        Write-Host "<- mower/$Device/gateway/cmd/ack ($($sw.ElapsedMilliseconds) ms round trip)"
        try { $reply | ConvertFrom-Json | ConvertTo-Json -Depth 5 } catch { $reply }
        $obj = $reply | ConvertFrom-Json -ErrorAction SilentlyContinue
        if ($obj.id -ne $Id) { Write-Warning "reply id '$($obj.id)' does not match '$Id'" }
        $last = if ($obj.ok) { 0 } else { 1 }
    } else {
        Write-Warning "no reply within $TimeoutS s $(if ($e) { ": $e" })"
        $last = 2
    }
}
Remove-Variable pw
exit $last

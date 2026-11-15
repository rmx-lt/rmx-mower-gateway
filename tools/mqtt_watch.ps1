# Print what the gateway publishes (wiki Mower-Protocol §3): status, state, telemetry,
# monitor samples, metric catalogue and acks, plus mower/status/server.
#
#   tools\mqtt_watch.ps1                    # 90 s, device mower-01
#   tools\mqtt_watch.ps1 -Seconds 300 -Device mower-02
#
# Subscribes as the MQTT login "app" (read mower/#) with mosquitto_sub; retained messages
# (status, state, catalogue) arrive first. The password is read like in mqtt_cmd.ps1
# (ssh rmx-vps, or $env:RMX_APP_MQTT_PW) and never printed. Needs the Mosquitto clients for Windows.
param(
    [int]$Seconds = 90,
    [string]$Device = 'mower-01'
)
$ErrorActionPreference = 'Stop'
$sub = 'C:\Program Files\mosquitto\mosquitto_sub.exe'
$ca = Join-Path $PSScriptRoot '..\certs\rmx-ca-bundle.pem' | Resolve-Path

$pw = $env:RMX_APP_MQTT_PW
if (-not $pw) {
    for ($try = 1; $try -le 3 -and -not $pw; $try++) {
        if ($try -gt 1) { Write-Host "ssh failed, retrying in 20 s ($try/3)"; Start-Sleep 20 }
        $pw = ssh -o BatchMode=yes -o ConnectTimeout=15 rmx-vps 'sudo awk ''/^app /{print $2}'' /opt/rmx-vps/services/mosquitto/data/credentials.txt'
    }
    if (-not $pw) { throw 'could not read the app password over ssh rmx-vps (3 tries)' }
}

$g = "mower/$Device/gateway"
$a = @('-h', 'mqtt.rmx.lt', '-p', '8883', '--cafile', "`"$ca`"", '-V', '5', '-v', '-q', '1',
       '-u', 'app', '-P', $pw, '-W', $Seconds,
       '-t', 'mower/status/server', '-t', "$g/status", '-t', "$g/state", '-t', "$g/telemetry",
       '-t', "$g/monitor", '-t', "$g/metrics/#", '-t', "$g/cmd/ack")
$p = Start-Process $sub -ArgumentList $a -NoNewWindow -PassThru
# mosquitto's -W timeout is unreliable on Windows: enforce it here too
if (-not $p.WaitForExit(($Seconds + 5) * 1000)) { $p.Kill(); $p.WaitForExit() }
Remove-Variable pw

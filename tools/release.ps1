# Build a signed gateway image with PlatformIO and upload it to the VPS for OTA.
#
#   tools\release.ps1                 # builds version.txt's version, uploads, prints the ota command
#   tools\release.ps1 -Send           # ... and sends it to the device (tools\mqtt_cmd.ps1 ota)
#   tools\release.ps1 -BuildOnly      # build, sign and check the image; no upload
#
# Needs PlatformIO (~\.platformio\penv\Scripts\pio.exe or `pio` on PATH), the signing key in
# keys\ota_signing_key.pem (wiki page Mower-Gateway, "Firmware updates") and `ssh rmx-vps`.
# Bump version.txt first: the device refuses the version it already runs.
param(
    [switch]$Send,
    [switch]$BuildOnly,
    [string]$Device = 'mower-01',
    [string]$RemoteDir = '/opt/rmx-vps/services/app/firmware'
)
$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot '..' | Resolve-Path
Push-Location $root
try {
    if (-not (Test-Path keys\ota_signing_key.pem)) { throw 'no keys\ota_signing_key.pem (see the wiki page Mower-Gateway, "Firmware updates")' }
    $pio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
    if (-not (Test-Path $pio)) { $pio = (Get-Command pio -ErrorAction Stop).Source }
    $version = (Get-Content version.txt -Raw).Trim()
    if ($version.Length -gt 31) { throw "version.txt '$version' is longer than 31 characters" }

    # CMake reads version.txt only when it configures: drop the cache so the image gets this version.
    Remove-Item .pio\build\gateway\CMakeCache.txt -ErrorAction SilentlyContinue
    & $pio run -e gateway
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }

    # sdkconfig.gateway keeps its old values over sdkconfig.defaults: refuse to ship an image that
    # still trusts the full Mozilla bundle (wiki page Mower-Gateway, "TLS trusts only ISRG Root X1 + X2").
    $cfg = Get-Content sdkconfig.gateway -Raw
    if ($cfg -notmatch '(?m)^CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_NONE=y' -or
        $cfg -notmatch '(?m)^CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH="certs/rmx-ca-bundle.pem"') {
        throw 'sdkconfig.gateway still uses the full CA bundle: copy the TLS lines from sdkconfig.defaults (wiki page Mower-Gateway)'
    }

    $bin = '.pio\build\gateway\firmware.bin'
    # The app description (esp_app_desc_t) starts at 0x20: magic 0xABCD5432, version at 0x30.
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $bin))
    if ([BitConverter]::ToUInt32($bytes, 0x20) -ne 0xABCD5432) { throw "$bin has no app description at 0x20" }
    $built = [Text.Encoding]::ASCII.GetString($bytes, 0x30, 32).TrimEnd([char]0)
    if ($built -ne $version) { throw "image version '$built' is not version.txt '$version'" }
    # sign_ota.py signs after every build; check it, so an unsigned image never ships.
    $espsecure = Join-Path $env:USERPROFILE '.platformio\packages\tool-esptoolpy\espsecure.py'
    $python = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
    & $python $espsecure verify_signature --version 2 --keyfile keys\ota_signing_key.pem $bin | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$bin is not signed with keys\ota_signing_key.pem" }

    $size = $bytes.Length
    $sha = (Get-FileHash $bin -Algorithm SHA256).Hash.ToLower()
    Write-Host "image $version, $size bytes, sha256 $sha, signed"
    if ($BuildOnly) { return }

    # New SSH connections to the VPS are sometimes dropped: retry a few times.
    $ok = $false
    for ($try = 1; $try -le 3 -and -not $ok; $try++) {
        if ($try -gt 1) { Write-Host "upload failed, retrying in 20 s ($try/3)"; Start-Sleep 20 }
        ssh -o BatchMode=yes -o ConnectTimeout=15 rmx-vps "mkdir -p $RemoteDir"
        if ($LASTEXITCODE -eq 0) {
            scp -o BatchMode=yes -o ConnectTimeout=15 $bin "rmx-vps:$RemoteDir/$version.bin"
            $ok = $LASTEXITCODE -eq 0
        }
    }
    if (-not $ok) { throw 'upload to rmx-vps failed (3 tries)' }
    Write-Host "uploaded: $RemoteDir/$version.bin (MQTT file name $version.bin)"

    $cmd = "tools\mqtt_cmd.ps1 ota -Version $version -Size $size -Device $Device"
    if ($Send) {
        & (Join-Path $PSScriptRoot 'mqtt_cmd.ps1') ota -Version $version -Size $size -Device $Device
        Write-Host 'the device acks "started", downloads, acks "done" and restarts; check with: tools\mqtt_cmd.ps1 status'
    } else {
        Write-Host "send it with: $cmd"
    }
} finally {
    Pop-Location
}

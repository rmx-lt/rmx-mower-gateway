# Write the device name and passwords into the board's NVS partition without printing the passwords.
#
#   tools\provision_nvs.ps1                 # device mower-01: asks for each password (hidden input), flashes COM6
#   tools\provision_nvs.ps1 -Device mower-02 -Port COM7
#   $env:RMX_MQTT_PW / RMX_WEB_PW / RMX_OTA_PW   # used instead of asking when set
#
# The device name is the MQTT login and the <name> in the topics mower/<name>/... The same image
# runs as any name: no rebuild. The legacy HTTPS login (web_pw, ota_pw) is -HttpsUser (default
# probe-device), independent of the device name.
# Keys (namespace "creds"): device, https_user, mqtt_pw (required), web_pw (required),
# ota_pw (optional: empty = no basic auth).
# Overwrites the whole nvs partition (0x9000, 24 KB), which holds nothing else the probe needs.
# `idf.py flash` leaves it alone, so run this once per board. Needs the ESP-IDF environment
# (. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1). The plain-text CSV only exists in a
# temp folder while this runs.
param(
    [string]$Device = 'mower-01',
    [string]$HttpsUser = 'probe-device',
    [string]$Port = 'COM6',
    [string]$Offset = '0x9000',
    [string]$Size = '0x6000'
)
$ErrorActionPreference = 'Stop'

function Get-Secret([string]$EnvName, [string]$Prompt, [bool]$Required) {
    $v = [Environment]::GetEnvironmentVariable($EnvName)
    if (-not $v) {
        $s = Read-Host -AsSecureString $Prompt
        $v = [Net.NetworkCredential]::new('', $s).Password
    }
    if ($Required -and -not $v) { throw "$Prompt is required" }
    return $v
}

# Check the environment before asking for any password.
if (-not $env:IDF_PATH) { throw 'IDF_PATH not set: load the ESP-IDF environment first (. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1)' }
$gen = Join-Path $env:IDF_PATH 'components\nvs_flash\nvs_partition_generator\nvs_partition_gen.py'
if (-not (Test-Path $gen)) { throw "nvs_partition_gen.py not found under IDF_PATH ($env:IDF_PATH)" }

# Same rule as the firmware (main/creds.c): the name goes into MQTT topics.
if ($Device -cnotmatch '^[A-Za-z0-9_-]{1,32}\z') { throw "-Device: 1-32 letters, digits, '-' or '_'" }
if ($HttpsUser -cnotmatch '^[A-Za-z0-9_-]{1,32}\z') { throw "-HttpsUser: 1-32 letters, digits, '-' or '_'" }

$mqtt = Get-Secret 'RMX_MQTT_PW' "MQTT password ($Device)" $true
$web = Get-Secret 'RMX_WEB_PW' "Web login password ($HttpsUser)" $true
$ota = Get-Secret 'RMX_OTA_PW' 'Firmware download password (empty = none)' $false

function Q([string]$s) { '"' + $s.Replace('"', '""') + '"' }
$dir = Join-Path ([IO.Path]::GetTempPath()) ('rmx-nvs-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $dir | Out-Null
try {
    $lines = @('key,type,encoding,value', 'creds,namespace,,', "device,data,string,$Device", "https_user,data,string,$HttpsUser",
               "mqtt_pw,data,string,$(Q $mqtt)", "web_pw,data,string,$(Q $web)")
    if ($ota) { $lines += "ota_pw,data,string,$(Q $ota)" }
    $csv = Join-Path $dir 'creds.csv'
    $bin = Join-Path $dir 'creds.bin'
    [IO.File]::WriteAllLines($csv, $lines)
    python -I $gen generate $csv $bin $Size | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'nvs_partition_gen failed' }
    python -I -m esptool --chip esp32 -p $Port write-flash $Offset $bin
    if ($LASTEXITCODE -ne 0) { throw 'flashing the NVS partition failed' }
} finally {
    Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "NVS written to $Port as device $Device (mqtt_pw, web_pw$(if ($ota) { ', ota_pw' }))."

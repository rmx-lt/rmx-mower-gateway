# Give the board its client certificate without touching the key or any other NVS key.
#
#   tools\provision_cert.ps1 -Name mower-01     # reset the board, get a CSR for that name, sign it, store the chain
#   tools\provision_cert.ps1 -Clear             # delete the stored certificate (the board stops connecting)
#   tools\provision_cert.ps1 -CaDir D:\ca       # another CA directory (default: ~\.rmx-device-ca)
#
# Steps: tools/cert_serial.py asks the board for a CSR with CN = -Name (the name is used only for this
# CSR: the board keeps no name, the certificate CN becomes its device name),
# device-ca.ps1 sign (blade-of-grass repo, wiki Device-CA) signs it and asks for the issuing passphrase,
# tools/cert_serial.py sends the chain (device + issuing certificate, public) over the serial port;
# the firmware checks it against its own key, stores it under the NVS key "chain" and restarts.
# The private key never leaves the board. -PassFile is passed through to device-ca.ps1 (a file with the
# issuing passphrase; for tests and for the owner's non-interactive signing only). Needs python with
# pyserial (the PlatformIO penv has it) and openssl on PATH (Git for Windows).
param(
    [string]$Name,
    [string]$Port = 'COM6',
    [string]$CaTool = 'C:\Projects\blade-of-grass\tools\device-ca\device-ca.ps1',
    [string]$CaDir,
    [string]$PassFile,
    [switch]$Clear
)
$ErrorActionPreference = 'Stop'

$py = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
if (-not (Test-Path $py)) { $py = 'python' }
$helper = Join-Path $PSScriptRoot 'cert_serial.py'

if ($Clear) {
    & $py -I $helper clear $Port
    if ($LASTEXITCODE -ne 0) { throw 'clearing the certificate failed' }
    Write-Host 'Certificate removed; the board does not connect until a new one is stored.'
    return
}

if ($Name -cnotmatch '^[A-Za-z0-9_-]{1,32}$') { throw '-Name <device name> is required (letters, digits, - and _, up to 32), e.g. mower-01' }
if (-not (Test-Path $CaTool)) { throw "device-ca.ps1 not found: $CaTool (-CaTool)" }
$dir = Join-Path ([IO.Path]::GetTempPath()) ('rmx-cert-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $dir | Out-Null
try {
    $csr = Join-Path $dir 'device.csr'
    & $py -I $helper csr $Port $csr $Name
    if ($LASTEXITCODE -ne 0) { throw 'reading the CSR failed' }
    $cn = ((& openssl req -in $csr -noout -subject) -replace '^subject\s*=\s*', '') -replace '^CN\s*=\s*', ''
    if ($cn -cnotmatch '^[A-Za-z0-9_-]{1,32}$') { throw "unexpected CSR subject: $cn" }
    Write-Host "Signing the CSR of $cn."
    $named = Join-Path $dir "$cn.csr"   # device-ca names its outputs after the CSR file
    Move-Item $csr $named
    $csr = $named

    $signArgs = @('-File', $CaTool, 'sign', $csr, '-OutDir', $dir)
    if ($CaDir) { $signArgs += @('-CaDir', $CaDir) }
    if ($PassFile) { $signArgs += @('-PassFile', $PassFile) }
    & pwsh @signArgs
    if ($LASTEXITCODE -ne 0) { throw 'signing failed' }

    $chain = Join-Path $dir "$cn.chain.pem"
    if (-not (Test-Path $chain)) { throw "no chain produced: $chain" }
    & $py -I $helper send $Port $chain
    if ($LASTEXITCODE -ne 0) { throw 'the board refused the certificate' }
} finally {
    Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "Done: $cn now holds its certificate and logs in with it once the broker accepts it."

# rmx-mower-gateway

Part of **Blade of Grass** (open-source robotic mower). Firmware for
the cellular link of the rmx-mower: an ESP32 + SIM7600G-H board (LilyGo T-SIM7600)
that connects over LTE to **rmx-server** and talks to **rmx-server-app** with
**MQTT over TLS**. ESP-IDF v6.1, esp_modem (PPP), mbedTLS, esp-mqtt.

- **Docs:** [Mower-Gateway](https://github.com/rmx-lt/blade-of-grass/wiki/Mower-Gateway) on the wiki
  (remote commands, OTA, error codes) and the
  [knowledge index](https://github.com/rmx-lt/blade-of-grass/wiki/Mower-Gateway-Knowledge)
  (board, modem, ESP-IDF, PPP, server contract, plan). The pages live in this repo's `wiki/` folder;
  rules: [Wiki-Policy](https://github.com/rmx-lt/blade-of-grass/blob/development/wiki/Wiki-Policy.md).
- **Work:** issues on the [Blade of Grass project](https://github.com/orgs/rmx-lt/projects/1).

## Build, configure, flash (PowerShell)

PlatformIO is the main tool; `idf.py` is a fallback (wiki Mower-Gateway-Tools).

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e gateway                  # build + sign (needs keys\ota_signing_key.pem)
& $pio run -e gateway -t menuconfig    # "rmx gateway": APN, broker URL (no logins, no passwords)
& $pio run -e gateway -t upload        # flash on COM6
tools\provision_cert.ps1 -Name mower-02   # new board: CSR for that name, sign it, store the certificate
python -I -u tools\serial_log.py COM6 120 --until STARTUP
```

The only identity is a client certificate; there are no passwords on the board. A new board:
flash, then `tools\provision_cert.ps1 -Name <name>`. The board makes its own ECDSA P-256 key on first
boot (NVS `creds/key`, never printed) and, until it has a certificate, only logs that it does not connect.
The script asks it over the serial port for a CSR with CN `<name>` (the name is used for that CSR only),
has the device CA sign it (`device-ca.ps1`, asks for the issuing passphrase) and writes the chain
(device + issuing certificate) back into `creds/chain`, leaving the key and every other NVS key alone.
Then the board logs in on port 8884 and takes its name (MQTT login and the `<name>` in `mower/<name>/...`)
from the certificate CN. Flashing (`-t upload`) and OTA leave NVS alone; erasing the flash removes it, key
included (a new key needs a new certificate).

## Layout

```
main/        the firmware: board, cell (esp_modem PPP), mqtt_link, remote_cmd, gnss, ota
tools/       at_bridge (AT console firmware), at_session.py (AT script), ppp_smoke (PPP + TLS test),
             mqtt_cmd.ps1 (remote commands), release.ps1 (signed OTA release),
             provision_cert.ps1 + cert_serial.py (client certificate provisioning)
version.txt  firmware version (PROJECT_VER), bump it for every OTA release
certs/       ISRG Root X1/X2 PEMs (public CA roots for rmx.lt's Let's Encrypt chain)
```

## License

Code and documentation are licensed under [Apache-2.0](LICENSE).

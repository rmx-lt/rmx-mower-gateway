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

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py set-target esp32     # first time only
idf.py menuconfig           # "rmx probe": APN, server URLs (no logins, no passwords)
idf.py build
idf.py -p COM6 -b 921600 flash
tools\provision_nvs.ps1      # board without a certificate yet: device name + MQTT password go to NVS
tools\provision_cert.ps1     # board with its CSR on the serial log: sign it and store the certificate
python -I -u tools\serial_log.py COM6 120 --until SELF-TEST
```

The board makes its own ECDSA P-256 key on first boot (NVS `creds/key`, never printed) and prints a
CSR between `=====RMX-CSR-BEGIN=====` / `END` lines. `tools\provision_cert.ps1` reads it, has the device CA
sign it and writes the chain (device + issuing certificate) back over the serial port into `creds/chain`,
leaving the key and every other NVS key alone. With a certificate the board logs in on port 8884 with no
password and takes its name (the MQTT login and the `<name>` in `mower/<name>/...`) from the certificate CN.
Without one it still uses the password login on 8883 (`tools\provision_nvs.ps1`: `device`, `mqtt_pw`; this
fallback goes away once the certificate login is live). `idf.py flash` and OTA leave NVS alone;
`idf.py erase-flash` removes it, key included (a new key needs a new certificate).

## Layout

```
main/        the firmware: board, cell (esp_modem PPP), mqtt_link, probe_proto, remote_cmd, gnss, ota
tools/       at_bridge (AT console firmware), at_session.py (AT script), ppp_smoke (PPP + TLS test),
             mqtt_cmd.ps1 (remote commands), release.ps1 (signed OTA release),
             provision_cert.ps1 + cert_serial.py (client certificate), provision_nvs.ps1 (password fallback)
version.txt  firmware version (PROJECT_VER), bump it for every OTA release
certs/       ISRG Root X1/X2 PEMs (public CA roots for rmx.lt's Let's Encrypt chain)
```

## License

Code and documentation are licensed under [Apache-2.0](LICENSE).

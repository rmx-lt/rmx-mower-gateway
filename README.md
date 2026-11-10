# rmx-mower-gateway

Part of **Blade of Grass** (open-source robotic mower). Firmware for
the cellular link of the rmx-mower: an ESP32 + SIM7600G-H board (LilyGo T-SIM7600)
that connects over LTE to **rmx-server** and talks to **rmx-server-app** with
**MQTT over TLS** and **HTTPS**. ESP-IDF v6.1, esp_modem (PPP), mbedTLS, esp-mqtt.

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
idf.py menuconfig           # "rmx probe": APN, MQTT and web logins (no passwords)
idf.py build
idf.py -p COM6 -b 921600 flash
tools\provision_nvs.ps1      # once per board: passwords go to NVS (hidden prompts), not into the image
python -I -u tools\serial_log.py COM6 120 --until SELF-TEST
```

The passwords (MQTT, web, firmware download) live only in the board's NVS partition (`creds` namespace).
`idf.py flash` and OTA leave them alone; `idf.py erase-flash` removes them.

## Layout

```
main/        the firmware: board, cell (esp_modem PPP), mqtt_link, https_api, probe_proto, remote_cmd, gnss, ota
tools/       at_bridge (AT console firmware), at_session.py (AT script), ppp_smoke (PPP + TLS test),
             mqtt_cmd.ps1 (remote commands), release.ps1 (signed OTA release)
version.txt  firmware version (PROJECT_VER), bump it for every OTA release
certs/       ISRG Root X1/X2 PEMs (public CA roots for rmx.lt's Let's Encrypt chain)
```

## License

Code and documentation are licensed under [Apache-2.0](LICENSE).

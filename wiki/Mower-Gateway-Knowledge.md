# Mower-Gateway-Knowledge: Knowledge index — LilyGo T-SIM7600 + ESP-IDF + rmx-server

Start here. Facts marked "verified" were measured on the real board on 2026-10-19.

Reusable board and chip facts (T-SIM7600 header pinout, free GPIOs, the ESP32-P4 board) live in the private ESP knowledge hub `jRmx0/esp-knowledge` (local `C:\Repos\esp-knowledge`, read its `INDEX.md`). Put new board or chip facts there; probe-specific knowledge stays on these `Mower-Gateway-*` pages.

| # | Page | Read it when you need… |
|---|---|---|
| 01 | [Mower-Gateway-Board-Hardware](Mower-Gateway-Board-Hardware) | pins, DIP switch, power, antennas, flash/PSRAM size, flashing tips |
| 02 | [Mower-Gateway-SIM7600](Mower-Gateway-SIM7600) | modem firmware, power sequence, registration, AT commands, modem-internal TLS/MQTT (verified) |
| 03 | [Mower-Gateway-ESP-IDF-Environment](Mower-Gateway-ESP-IDF-Environment) | toolchain paths, build/flash commands, IDF v6 changes, sdkconfig for this board |
| 04 | [Mower-Gateway-PPP](Mower-Gateway-PPP) | esp_modem + PPP: correct DTE config, startup sequence, CMUX, robustness |
| 05 | [Mower-Gateway-Server-Contract](Mower-Gateway-Server-Contract) | endpoints, cert chain, Mosquitto ACL, probe MQTT protocol, firmware chunks |
| 06 | [Mower-Gateway-Plan](Mower-Gateway-Plan) | what is proven (incl. the probe firmware), firmware layout, decisions, risks |
| — | [Mower-Gateway-Sources](Mower-Gateway-Sources) | where every fact came from (local repos, PDFs, web) |
| — | [Mower-Gateway-Tools](Mower-Gateway-Tools) | AT bridge firmware, AT script, PPP smoke test |

## Quick facts (cheat sheet)

```
Board      LilyGo T-SIM7600 (ESP32), ESP32-WROVER-E N16R8: 16 MB flash, 8 MB PSRAM (4 MB mapped)
Modem      SIM7600G-H, fw LE20B04SIM7600G22, 115200 8N1, no RTS/CTS
UART       ESP TX = GPIO27, ESP RX = GPIO26 (UART1)
Control    PWRKEY GPIO4 (HIGH 500 ms = on/off toggle), FLIGHT GPIO25 = HIGH, DTR GPIO32 = LOW,
           RI GPIO33 (in), STATUS GPIO34 (in, 1 = on), LED GPIO12
PC         COM6 (CH9102); ESP-IDF v6.1: . C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
SIM/APN    Tele2 LT "PILDYK", APN "internet", LTE band 1, CGNAT IP
Data path  esp_modem 2.1.0 PPP (ESP_MODEM_DCE_SIM7600) + mbedTLS + espressif/mqtt
Server     mqtts://mqtt.rmx.lt:8884 (client certificate, client id := CN; 8883 is the password listener for the app and PC tools). Firmware files are the same *.bin names as /fw/probe/, served as MQTT chunks.
Trust      ISRG Root X1 + X2 (LE Gen-Y ECDSA chain)
Proven     PPP → MQTTS. tools/ppp_smoke still does one HTTPS GET /probe/health; the gateway firmware does not.
Firmware   main/ (repo root): MQTT login with the client certificate (name = CN) on mower/<name>/req|res and mower/<name>/fw/…
Logins     certificate only: board-made P-256 key + chain in NVS (tools/provision_cert.ps1 -Name <name>), name = CN, port 8884, no password anywhere on the board (live-verified 2026-11-11). See Mower-Gateway "Identity"
Commands   PC → mower/<name>/cmd → device → …/cmd/ack (tools/mqtt_cmd.ps1, login app) — verified
OTA        `ota` command → MQTT file <version>.bin, signed (RSA-3072, no Secure Boot), whole-image SHA-256, rollback. Verified 2026-11-10: 0.3.0 USB flash, unknown file `not_found`, reset at 10% then clean retry, 0.3.1 confirmed on ota_1 (97 s, 729088 bytes).
GNSS       "gps": true on create; AT via esp_modem_pause_net (~2 s, links survive) — fix and no-fix verified
```

## Local reference material

| Path | Content |
|---|---|
| `C:\Repos\LilyGo-Modem-Series` | vendor repo: docs, schematic `schematic/esp32/T-SIM7600X-V1.3.pdf`, SIMCom PDFs in `datasheet/SIM7600X/`, Arduino examples (`examples/*/utilities.h` has pin maps), TinyGSM fork (`lib/TinyGSM/src/TinyGsmClientSIM7600.h`, `TinyGsmMqttA76xx.h`, `TinyGsmHttpsComm.h` = proven AT sequences) |
| `C:\Repos\esp-protocols\components\esp_modem` | esp_modem source, docs (`docs/`), examples `pppos_client`, `simple_cmux_client`, `modem_console`, `modem_tcp_client` |
| `C:\esp\v6.1\esp-idf` | ESP-IDF; cert bundle `components/mbedtls/esp_crt_bundle/cacrt_all.pem` |
| `C:\Projects\blade-of-grass\rmx-server` | `CLAUDE.md`, `rmx-server-app` (server code + protocol), `rmx-server-infra` (Caddy, Mosquitto config) |

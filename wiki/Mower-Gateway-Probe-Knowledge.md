# Knowledge index — LilyGo T-SIM7600 + ESP-IDF + rmx-server

Start here. Facts marked "verified" were measured on the real board on 2026-10-19.

Reusable board and chip facts (T-SIM7600 header pinout, free GPIOs, the ESP32-P4 board) live in the private ESP knowledge hub `jRmx0/esp-knowledge` (local `C:\Repos\esp-knowledge`, read its `INDEX.md`). Put new board or chip facts there; probe-specific knowledge stays on these `Mower-Gateway-Probe-*` pages.

| # | Page | Read it when you need… |
|---|---|---|
| 01 | [Mower-Gateway-Probe-Board-Hardware](Mower-Gateway-Probe-Board-Hardware) | pins, DIP switch, power, antennas, flash/PSRAM size, flashing tips |
| 02 | [Mower-Gateway-Probe-SIM7600](Mower-Gateway-Probe-SIM7600) | modem firmware, power sequence, registration, AT commands, modem-internal TLS/MQTT (verified) |
| 03 | [Mower-Gateway-Probe-ESP-IDF-Environment](Mower-Gateway-Probe-ESP-IDF-Environment) | toolchain paths, build/flash commands, IDF v6 changes, sdkconfig for this board |
| 04 | [Mower-Gateway-Probe-PPP](Mower-Gateway-Probe-PPP) | esp_modem + PPP: correct DTE config, startup sequence, CMUX, robustness |
| 05 | [Mower-Gateway-Probe-Server-Contract](Mower-Gateway-Probe-Server-Contract) | endpoints, cert chain, Mosquitto ACL, probe MQTT/HTTPS protocol, cookie + Origin rules |
| 06 | [Mower-Gateway-Probe-Plan](Mower-Gateway-Probe-Plan) | what is proven (incl. the probe firmware), firmware layout, decisions, risks |
| — | [Mower-Gateway-Probe-Sources](Mower-Gateway-Probe-Sources) | where every fact came from (local repos, PDFs, web) |
| — | [Mower-Gateway-Probe-Tools](Mower-Gateway-Probe-Tools) | AT bridge firmware, AT script, PPP smoke test |

## Quick facts (cheat sheet)

```
Board      LilyGo T-SIM7600 (ESP32), ESP32-WROVER-E N16R8: 16 MB flash, 8 MB PSRAM (4 MB mapped)
Modem      SIM7600G-H, fw LE20B04SIM7600G22, 115200 8N1, no RTS/CTS
UART       ESP TX = GPIO27, ESP RX = GPIO26 (UART1)
Control    PWRKEY GPIO4 (HIGH 500 ms = on/off toggle), FLIGHT GPIO25 = HIGH, DTR GPIO32 = LOW,
           RI GPIO33 (in), STATUS GPIO34 (in, 1 = on), LED GPIO12
PC         COM6 (CH9102); ESP-IDF v6.1: . C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
SIM/APN    Tele2 LT "PILDYK", APN "internet", LTE band 1, CGNAT IP
Data path  esp_modem 2.1.0 PPP (ESP_MODEM_DCE_SIM7600) + mbedTLS + espressif/mqtt + esp_http_client
Server     https://api.rmx.lt/probe/…  ·  mqtts://mqtt.rmx.lt:8883 (login required, client id := username)
Trust      ISRG Root X1 + X2 (LE Gen-Y ECDSA chain); SNI required for api.rmx.lt
Proven     PPP → HTTPS 200 /probe/health → MQTTS CONNACK (fake login refused) — tools/ppp_smoke
Firmware   main/ (repo root): login probe-device on MQTT + HTTPS, create/list both ways — verified
Logins     MQTT probe-device, web probe-device; passwords only in gitignored sdkconfig
Commands   PC → mower/probe-device/cmd → device → …/cmd/ack (tools/mqtt_cmd.ps1, login app) — verified
OTA        `ota` command → api.rmx.lt/fw/probe/<ver>.bin, signed (RSA-3072, no Secure Boot), rollback — 0.2.0 → 0.2.1 over LTE verified 2026-10-22 (~3 min)
GNSS       "gps": true on create/https_create; AT via esp_modem_pause_net (~2 s, links survive) — fix and no-fix verified
```

## Local reference material

| Path | Content |
|---|---|
| `C:\Repos\LilyGo-Modem-Series` | vendor repo: docs, schematic `schematic/esp32/T-SIM7600X-V1.3.pdf`, SIMCom PDFs in `datasheet/SIM7600X/`, Arduino examples (`examples/*/utilities.h` has pin maps), TinyGSM fork (`lib/TinyGSM/src/TinyGsmClientSIM7600.h`, `TinyGsmMqttA76xx.h`, `TinyGsmHttpsComm.h` = proven AT sequences) |
| `C:\Repos\esp-protocols\components\esp_modem` | esp_modem source, docs (`docs/`), examples `pppos_client`, `simple_cmux_client`, `modem_console`, `modem_tcp_client` |
| `C:\esp\v6.1\esp-idf` | ESP-IDF; cert bundle `components/mbedtls/esp_crt_bundle/cacrt_all.pem` |
| `C:\Projects\blade-of-grass\rmx-server` | `CLAUDE.md`, `rmx-server-app-probe` (server code + protocol), `rmx-server-infra` (Caddy, Mosquitto config) |

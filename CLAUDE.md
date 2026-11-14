# rmx-mower-gateway — notes for agents

Project-wide agent rules live in the meta repo's [CLAUDE.md](https://github.com/rmx-lt/blade-of-grass/blob/development/CLAUDE.md); read it first. This file only adds what is specific to this repo.

Goal: ESP-IDF firmware on a LilyGo T-SIM7600 (ESP32 + SIM7600G-H) that reaches rmx-server over
LTE and talks to rmx-server-app with MQTT over TLS (`mqtt.rmx.lt:8883`), including firmware
download. The firmware has no HTTPS client.

**Read `wiki/Mower-Gateway-Knowledge.md` first** (local, in this repo; online:
https://github.com/rmx-lt/blade-of-grass/wiki/Mower-Gateway-Knowledge). It indexes
verified facts about the board, the modem, the ESP-IDF setup, esp_modem/PPP, and the server contract
(pages `Mower-Gateway-*`). The firmware is in `main/` (verified on hardware); the minimal reference is
`tools/ppp_smoke/main/ppp_smoke.c`. Update the `wiki/` pages in the same PR when you learn something new or find a fact
wrong (mark what you verified on hardware and when), per
[Wiki-Policy](https://github.com/rmx-lt/blade-of-grass/blob/development/wiki/Wiki-Policy.md).

Work and open questions are GitHub issues in this repo, on the Blade of Grass project
(https://github.com/orgs/rmx-lt/projects/1), not notes in markdown files.

Reusable board, chip and ESP-IDF knowledge also lives in the central hub `C:\Repos\esp-knowledge`
(private repo `jRmx0/esp-knowledge`; read its `INDEX.md`). Its `boards/t-sim7600.md` has the header pinout and
free GPIOs; put new board or chip facts there, and keep gateway-specific knowledge in the wiki (`Mower-Gateway-*` pages).

## Environment

- Windows, PowerShell primary. ESP-IDF v6.1: `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`, then `idf.py`.
- Two build paths: `idf.py` (reference) and PlatformIO (`~/.platformio/penv/Scripts/pio.exe run -e gateway`; `platformio.ini`, pinned platform 7.1.0 = ESP-IDF 6.1.0). PlatformIO needs `keys/ota_signing_key.pem` (`sign_ota.py` signs the image) and writes `sdkconfig.gateway` (gitignored, holds credentials).
- Board on **COM6** (CH9102). Opening the port resets the ESP32 (not the modem).
- No interactive `idf.py monitor` for agents: read the serial log with a pyserial script (see the wiki page `Mower-Gateway-Tools`).
- Related repos: `C:\Repos\esp-knowledge`, `C:\Repos\LilyGo-Modem-Series`, `C:\Repos\esp-protocols`, `C:\esp\v6.1\esp-idf`,
  `C:\Projects\blade-of-grass\rmx-server\rmx-server-infra` (its `CLAUDE.md` has the VPS rules).

## Must-know pitfalls

- esp_modem's default DTE pins (tx 25, rx 26, rts 27, cts 23) are wrong here: use TX 27, RX 26, no RTS/CTS.
- FLIGHT (GPIO25) HIGH, DTR (GPIO32) LOW. PWRKEY (GPIO4) HIGH 500 ms toggles power: send `AT` first.
- MQTT and cJSON are not in ESP-IDF v6: use `espressif/mqtt`, `espressif/cjson`.
- Broker forces client id = username; ACL limits each login to its topics. Forbidden SUB/PUB fail silently.
- Firmware chunks are about 4 KB. `mqtt_link` sets the esp-mqtt receive buffer to 8192 bytes and still reassembles `MQTT_EVENT_DATA` fragments.

## Rules

- This repo is public (Apache-2.0): never commit passwords, tokens, IMEI/ICCID/IMSI, or personal data.
  The board makes its own key (NVS, never printed); the certificate goes in via `tools/provision_cert.ps1`. Fallback passwords go in NVS only (`tools/provision_nvs.ps1`), never in `sdkconfig`. Never run provision_nvs.ps1 on a board that has a key: it wipes NVS. Server secrets stay on the VPS; never print them.
- VPS changes (new MQTT users, ACL) follow `rmx-server-infra/CLAUDE.md` and need the user's OK.
- Flashing the board is fine (it's the dev board for this project); say what you flashed.

# Mower-Gateway: rmx-mower-gateway

Part of **Blade of Grass** (open-source robotic mower). Firmware for
the cellular link of the rmx-mower: an ESP32 + SIM7600G-H board that connects over LTE
to **rmx-server** and talks to **rmx-server-app** with **MQTT over TLS** only.

- Hardware: LilyGo T-SIM7600 (ESP32-WROVER-E N16R8, SIM7600G-H)
- Framework: ESP-IDF v6.1, esp_modem (PPP), mbedTLS, esp-mqtt
- Server: `mqtts://mqtt.rmx.lt:8883`

## Status

Probe firmware working on the hardware: over LTE (PPP) it logs in to the broker as the
NVS device name (default `mower-01`), sends `create`/`list` on `mower/<name>/req` and gets
the answers on `mower/<name>/res`. It takes remote commands from the PC on
`mower/<name>/cmd` and replies on `…/cmd/ack`. A new image is requested as MQTT chunks on
`mower/<name>/fw/req` and written from `mower/<name>/fw/chunk`. The firmware has no HTTPS
client. A USB flash of this image (0.2.4, 2026-11-10) passed the MQTT self-test. An
over-the-air run is not measured yet: the server was not serving firmware
([rmx-mower-gateway#44](https://github.com/rmx-lt/rmx-mower-gateway/issues/44)). Details:
[Mower-Gateway-Plan](Mower-Gateway-Plan).

## Build, configure, flash (PowerShell)

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py set-target esp32     # first time only
idf.py menuconfig           # "rmx probe": APN, server URLs (no logins, no passwords)
idf.py build
idf.py -p COM6 -b 921600 flash
tools\provision_nvs.ps1      # once per board: device name + passwords go to NVS, not into the image
python -I -u tools\serial_log.py COM6 120 --until SELF-TEST
```

The device name and the MQTT password live only in the board's NVS partition (`creds`
namespace: `device`, `mqtt_pw`), so one image fits every board:
`tools\provision_nvs.ps1 -Device <name>` switches the identity without a rebuild. The name
is the MQTT login and the `<name>` in `mower/<name>/req|res|cmd|fw|…`; without a `device`
key the board boots as `mower-01`. `idf.py flash` and OTA leave NVS alone; `idf.py erase-flash`
removes it. Running `provision_nvs.ps1` replaces the whole NVS partition, so older keys
(`https_user`, `web_pw`, `ota_pw`) disappear. The firmware does not read them.

TLS trusts only ISRG Root X1 + X2 ([`certs/rmx-ca-bundle.pem`](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/certs/rmx-ca-bundle.pem)), not
ESP-IDF's full Mozilla bundle. An `sdkconfig` made before 0.2.3 still has the full bundle:
in menuconfig, *Component config → mbedTLS → Certificate Bundle*, set *Default certificate
bundle options* to *Use no default certificates bundle* and enable *Add custom certificates
to the default bundle* with path `certs/rmx-ca-bundle.pem` (or copy the four TLS lines from
`sdkconfig.defaults` into `sdkconfig`).

## Send a command from the PC

```powershell
tools\mqtt_cmd.ps1 ping
tools\mqtt_cmd.ps1 status
tools\mqtt_cmd.ps1 create -Value "hello from the PC"
tools\mqtt_cmd.ps1 create -Gps          # value = GPS location, or "gps unavailable: <why>"
tools\mqtt_cmd.ps1 delete -RecordId 41
tools\mqtt_cmd.ps1 restart
tools\mqtt_cmd.ps1 ota -Version 0.2.5 -Size 851234   # file = 0.2.5.bin; see "Firmware updates"
```

Commands: `ping status create list delete ota restart`. Each carries
`expires_at` (now + `-TtlS`, default 60 s). `-Gps` (create) sends the device's
GNSS location instead of a value; without a fix it sends `gps unavailable: …` and says why.
Needs the Mosquitto clients for Windows and `ssh rmx-vps`.

## Firmware updates (OTA)

The device downloads a new image as MQTT chunks into the spare app slot (`partitions.csv`:
two 4 MB slots), checks it, boots it, and keeps it only if it reaches the MQTT broker within
`CONFIG_PROBE_OTA_CONFIRM_S` (300 s). Otherwise the previous image boots again. Code:
[`main/ota.c`](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/main/ota.c).
The byte layout is wiki [Server-App](Server-App#firmware-over-mqtt). The device side is
[Mower-Gateway-Server-Contract](Mower-Gateway-Server-Contract).

- **Signed images.** OTA images must be signed with our RSA-3072 key (Secure Boot V2 scheme,
  `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` and `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`).
  `esp_ota_end` checks that signature before `esp_ota_set_boot_partition`. The running
  firmware only accepts an update signed with the same key as itself. There is no hardware
  Secure Boot: no eFuses are burned, and USB flashing works as before.
- **Whole-image SHA-256.** Every chunk header carries the SHA-256 of the file. The device
  hashes the bytes it wrote and compares them before `esp_ota_set_boot_partition`.
- **File name.** The `ota` command's `version` selects `<version>.bin`, the same files Caddy
  serves at `https://api.rmx.lt/fw/probe/<version>.bin` (wiki
  [Server-Infra](Server-Infra#firmware-images-ota) §10). An optional `file` (or the last
  segment of a legacy `url`) names the file instead. The device does not open HTTPS.
- **Report.** Once a new image has confirmed itself, it tells the server once: a retained
  `{"version","previous","partition","time"}` on `mower/<name>/firmware`, and a probe
  record `firmware updated to <version> (from <previous>, <slot>)`.
- **Retries.** One chunk request is in flight. A timeout retries that chunk (same request id,
  up to 3 more times). Bytes already written stay, so a reconnect resumes at the next chunk.
  A reset of the board drops the in-memory progress; the next `ota` command starts again.
  The running firmware is untouched until the whole image checks out.

One-time setup (the bootloader can't be updated over the air, so this needs USB):

```powershell
mkdir keys
espsecure generate-signing-key --version 2 --scheme rsa3072 keys\ota_signing_key.pem
# back up keys\ota_signing_key.pem offline: without it, no more OTA (only a USB flash with a new key)
idf.py -p COM6 -b 921600 flash
tools\provision_nvs.ps1      # device name + MQTT password
```

An existing `sdkconfig` keeps its old values: add the OTA block from `sdkconfig.defaults`
to it (or set the same options in menuconfig) before that build.

Each release:

```powershell
# bump version.txt (the device refuses the version it already runs)
tools\release.ps1 -Send    # build + sign, upload to the VPS, send the ota command
tools\mqtt_cmd.ps1 status  # result.version, result.ota (state, progress, error), rolled_back_from
```

The `ota` command: `{"id":"…","cmd":"ota","version":"0.2.5","file":"0.2.5.bin","size":851234}`.
`file` defaults to `<version>.bin`. It acks `started` at once, then `done` before the restart,
or an error: `busy`, `same_version`, `low_signal`, `bad_image` (wrong project or version, bad
signature or SHA-256), `bad_size`, `download_failed`, `not_found`, `bad_request`, `internal`,
`failed`. The signed PlatformIO image of 0.2.4 (2026-11-10) is 729088 bytes, plus a 64-byte
header on each 4096-byte chunk. The esp-mqtt receive buffer is 8192 bytes so that frame
arrives in one event; a fragmented `MQTT_EVENT_DATA` is still reassembled.

## Layout

```
main/        the probe firmware: board, cell (esp_modem PPP), mqtt_link, probe_proto, remote_cmd, gnss, ota
tools/       at_bridge (AT console firmware), at_session.py (AT script), ppp_smoke (PPP + TLS test),
             mqtt_cmd.ps1 (remote commands), release.ps1 (signed OTA release)
version.txt  firmware version (PROJECT_VER), bump it for every OTA release
certs/       ISRG Root X1/X2 PEMs (public CA roots for rmx.lt's Let's Encrypt chain)
```

## License

[Apache-2.0](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/LICENSE).

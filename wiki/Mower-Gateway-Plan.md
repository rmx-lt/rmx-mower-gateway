# Mower-Gateway-Plan: Plan for the probe firmware, risks, open questions

## Goal

`rmx-mower-gateway`: ESP-IDF firmware on the T-SIM7600 that connects over LTE to
rmx-server and talks to **rmx-server-app** over **MQTT over TLS** (`mqtt.rmx.lt:8883`), including
firmware download. It is the device-side rehearsal of the future rmx-mower link. The firmware
has no HTTPS client.

## What is already proven on the hardware (2026-10-19)

- [x] ESP32 ⇄ SIM7600 UART on GPIO 27 (TX) / 26 (RX), 115200, no flow control
- [x] Modem power/state handling (already-on detection, FLIGHT high, DTR low)
- [x] SIM ready, LTE registration on Tele2 LT (APN `internet`)
- [x] PPP with esp_modem 2.1.0 on ESP-IDF v6.1, IP in ~160 ms after dial
- [x] HTTPS GET `/probe/health` → 200, mbedTLS + cert bundle verification
- [x] Client certificate (issue 43): key generation, CSR, chain write/clear and password fallback verified on the board 2026-11-10; image +12 KB flash (741376 bytes signed), +0.5 KB static RAM. Open: live login on 8884 with the real CA, then drop `mqtt_pw` and `provision_nvs.ps1`.
- [x] MQTTS TLS handshake to 8883 + MQTT CONNECT reaches Mosquitto (fake login → CONNACK 5)
- [x] Recovery after an ESP reset while the modem is in PPP
- [x] Fallback: modem-internal TLS (AT+CCH*) with CA verification to both hosts

### The probe firmware itself (repo root, `main/`) — verified 2026-10-19, 2 runs

- [x] MQTT login `probe-device` over TLS (CONNACK in ~3.6 s incl. handshake), SUBACK on
      `probe/res/probe-device`, retained `probe/status/server` = `online` received
- [x] MQTT `create` on `probe/req/probe-device` → answer on `probe/res/probe-device` in ~240 ms
      (`connection_type: mqtt`), plus the `probe/events/records` event; `list` in ~370 ms
- [x] MQTT create/list on `mower/<name>/req|res`. The gateway firmware has no HTTPS client
      (removed with firmware-over-MQTT). `tools/ppp_smoke` still checks `GET /probe/health`.
- [x] ESP reset while the modem was in PPP → next boot connects normally (PPP in ~150 ms)
- [x] 3 min online afterwards: PPP and MQTT stayed up (keepalive 60 s), heap stable
      (4.39 MB free, 284 KB internal)
- App image on that date: 833 KB in a 4 MB OTA slot. The 2026-11-10 PlatformIO `probe`
  image (0.3.0, and the 0.3.1 OTA target of the same tree) is RAM 20752/327680, flash
  706493/4194304; the signed `firmware.bin` is 729088 bytes.

### Remote commands over MQTT — verified 2026-10-19

The device subscribes `mower/probe-device/cmd` (QoS 1) and answers on
`mower/probe-device/cmd/ack`: the planned mower command path, rehearsed. Code: `main/remote_cmd.c`;
PC sender: `tools/mqtt_cmd.ps1` (login `app`, the backend role, via `mosquitto_rr`).

```jsonc
// in:  mower/probe-device/cmd
{"id":"pc-1a2b","cmd":"create","value":"hi","expires_at":1792443479}
// out: mower/probe-device/cmd/ack
{"id":"pc-1a2b","cmd":"create","ok":true,"result":{"kind":"record","record_id":41,"connection_type":"mqtt"},"ms":479,"time":1792443419}
{"id":"pc-1a2b","cmd":"create","ok":false,"error":"expired","message":"…","ms":0,"time":…}
```

- Commands: `ping`, `status` (uptime, PPP, rssi, heap, clock), `create`/`list`/`delete` (forwarded
  to the probe server over MQTT), `ota`, `restart` (acks, then leaves PPP and restarts).
- Errors: `bad_request`, `unknown_cmd`, `expired`, `no_clock`, `timeout`, `failed`, or the probe
  server's own code. Duplicate ids (QoS 1 redelivery; last 8 kept) are ignored without a reply.
- `expires_at` (unix s, optional): refused when past, or when the clock isn't set. The clock comes
  from SNTP (`pool.ntp.org`) over PPP right after boot (worked first try, < 4 s).
- Measured PC → device → PC round trip (incl. a fresh TLS connect of `mosquitto_rr`): ~0.5–1 s;
  `create` ~0.5 s on the device. After `restart` the device was back and answering in ~30 s.
- Commands run on their own task (queue of 4): the MQTT task must never block, because a
  forwarded request waits for its response on that same task.
- `"gps": true` on `create` (`tools/mqtt_cmd.ps1 -Gps`, verified 2026-10-20): the value
  sent is the GNSS location as a JSON string, e.g.
  `{"gps":"fix","lat":…,"lon":…,"alt":…,"utc":"2026-10-20T09:19:18Z","sats":9,"hdop":0.8,"mode":"3D"}`,
  or `gps unavailable: <why>` (no fix (searching N s), GNSS was off / turned on now, AT+CGPS? / AT+CGNSSINFO
  error|timeout, PPP down, modem busy, +++ failed). The ack `result` gets `"gps"` (the status) and
  `"gps_fix"`. `gps` together with `value` → `bad_request`. Code: `main/gnss.c`; AT access by pausing PPP
  for ~2 s (04). GNSS is turned on at boot (`gnss_start()`).
  Verified: no-fix path over MQTT (record 50, 52), records on the server hold
  `gps unavailable: no fix (searching 20 s)`; MQTT/PPP stayed up; `create` with gps ~2.2–2.3 s on the
  device. Fix path verified the same day with the board at a
  window: `create` → record 60 with the JSON location (12 sats,
  HDOP 1.0), ack `"gps_fix":true,"gps":"fix (2D, 12 sats, <utc>)"`, ~2.2 s on the device.
  Indoors (antenna connected): no fix after 18 min (record 54).
- `mosquitto_rr` 2.0.20 has no `-f`; it silently sent an empty payload. Use `-m` (quote-escaped).
- Using `app` from the PC: on 8883 its client id is forced to `app`, so a real backend on that
  login would be kicked while the script runs (no backend runs yet).

### Firmware updates (OTA)

`main/ota.c` downloads `<version>.bin` as MQTT chunks (the same file names as `/fw/probe/`),
one request in flight. A timeout retries that chunk (3 retries, same request id) and the next
index resumes while the task is still running. A board reset drops that progress; the next
`ota` command starts clean. Checks, in order: app header (project, the requested version, not
the running one), size, whole-image SHA-256 against the chunk header, then `esp_ota_end`
(the RSA-3072 signature, `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`) and only then
`esp_ota_set_boot_partition`. See [Mower-Gateway → Firmware updates](Mower-Gateway#firmware-updates-ota).

- Rollback: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. A new image confirms itself on the first
  MQTT CONNACK; without it within `CONFIG_PROBE_OTA_CONFIRM_S` (300 s) it rolls back.
- Signed apps without Secure Boot (decided by the user 2026-10-22: no Secure Boot, no eFuses):
  RSA-3072, key in gitignored `keys/ota_signing_key.pem`. Needs `CONFIG_ESP32_REV_MIN_3`
  (the board is v3.1). On 2026-11-10 `espsecure verify_signature --version 2` accepted the
  signed 729088-byte image and rejected a copy with one byte flipped (digest mismatch).
  That is the same check `esp_ota_end` runs. A tampered image on the device is not measured yet.
- Bootloader from the 2026-10-22 build: **0x6d20 bytes, only 0x2e0 (736 B) free** below the
  partition table at 0x8000. Any more bootloader features would need a lower bootloader log
  level or moving the partition table (= new layout, USB).
- HTTPS OTA, verified 2026-10-22, before this path: USB flash of 0.2.0 → `ota_0`, then
  0.2.0 → 0.2.1 (856064 bytes) over HTTPS, confirmed on `ota_1`. A second `ota` of 0.2.1
  returned `same_version`.
- MQTT OTA, verified 2026-11-10 on the board (login `mower-01`). USB flash of 0.3.0
  booted `ota_0`, state valid, MQTT self-test passed. `ota` of `missing.bin` acked
  `started`, then `not_found` (`unknown firmware file`); `status` stayed version 0.3.0,
  partition `ota_0`, confirmed. `ota` of `0.3.1.bin` (729088 bytes, 178 chunks of 4096)
  reached 10% (chunk 18) and a reset came back as 0.3.0 on `ota_0`. The same command
  again downloaded from chunk 1, logged `image SHA-256 matches the chunk header`,
  `update to 0.3.1 written in 97 s`, rebooted to `ota_1` (state pending), and logged
  `image 0.3.1 confirmed` on the first MQTT connect. `status`: version 0.3.1, partition
  `ota_1`, confirmed, ota idle. The server `info` for `0.3.1.bin` matched that size and
  the file's SHA-256. A tampered image was not sent to the device.
- Other failure paths (crash on boot, wrong key, power cut) are not verified on the board:
  [rmx-mower-gateway#15](https://github.com/rmx-lt/rmx-mower-gateway/issues/15).

Open work is tracked as issues on the [Blade of Grass project](https://github.com/orgs/rmx-lt/projects/1):
reconnect under real signal loss ([#16](https://github.com/rmx-lt/rmx-mower-gateway/issues/16)),
certificate date checks after SNTP ([#18](https://github.com/rmx-lt/rmx-mower-gateway/issues/18)), CMUX / power saving / modem MQTT AT path ([#19](https://github.com/rmx-lt/rmx-mower-gateway/issues/19)),
OTA failure paths ([#15](https://github.com/rmx-lt/rmx-mower-gateway/issues/15)).

## Firmware structure (implemented)

```
main/
  board.c/.h        pins, modem power on/off, STATUS
  cell.c/.h         esp_modem DTE/DCE, registration wait, PPP up/down, reconnect loop, events
  mqtt_link.c/.h    esp-mqtt client: mqtts://mqtt.rmx.lt:8883, LWT, subscribe, request/response, fw/req
  gnss.c/.h         SIM7600 GNSS over AT (via cell_with_command_mode): on at boot, read on demand
  remote_cmd.c/.h   commands from mower/<user>/cmd, acks on …/cmd/ack
  ota.c/.h          MQTT chunk download, image checks, first-boot confirm / rollback
  probe_proto.c/.h  JSON envelopes (espressif/cjson), request_id generation
  app_main.c        sequence: board → cell → mqtt test → loop
idf_component.yml   espressif/esp_modem ^2.1, espressif/mqtt ^1.1, espressif/cjson ^1.7.19
Kconfig.projbuild   APN, server URL, default device name (no passwords)
creds.c/.h          device name (certificate CN, else NVS device) + MQTT password from NVS (creds: device, mqtt_pw)
devcert.c/.h        P-256 key + CSR + certificate chain in NVS (creds: key, chain), serial provisioning commands
partitions.csv      16 MB layout with OTA slots
```

Start from [`tools/ppp_smoke/main/ppp_smoke.c`](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/tools/ppp_smoke/main/ppp_smoke.c).

MQTT client settings to start with:
- `broker.address.uri = "mqtts://mqtt.rmx.lt:8883"`, `broker.verification.crt_bundle_attach = esp_crt_bundle_attach`
  (or `certificate = <isrg-root-x1+x2 PEM>` to trust only those roots)
- `credentials.username/password` from NVS (username = device name); client id is forced to the username by the broker
- `session.keepalive = 60`, `session.last_will` → `mower/<name>/status` (`mower-01` owns `mower/mower-01/#`)
- `buffer.size = 8192` so one firmware chunk fits; `buffer.out_size = 2048`
- `buffer.size` ≥ 16–64 KB if the device sends `list`
- QoS 1 for requests; match responses by `request_id`

## Decisions (answered by the user 2026-10-19)

1. **MQTT login**: a dedicated Mosquitto user **`probe-device`** (rmx-server-infra branch
   `probe-device-login`). ACL: write `probe/req/probe-device`; read `probe/res/probe-device`,
   `probe/events/#`, `probe/status/#` (plus the `mower/probe-device/#` pattern every login gets).
2. **HTTPS login**: a dedicated web user **`probe-device`** (`probe-server create-user`).
3. **Secrets**: the board's key and certificate chain (`creds/key`, `creds/chain`, `tools/provision_cert.ps1`) replace the passwords (issue 43; `mqtt_pw` stays as fallback until the live switch). Until then the passwords live in NVS, written by `tools/provision_nvs.ps1`
   (hidden prompts or `RMX_MQTT_PW` / `RMX_WEB_PW` / `RMX_OTA_PW`); plain-text copies only in the VPS
   `credentials.txt` files. Before [#17](https://github.com/rmx-lt/rmx-mower-gateway/issues/17) they sat in the gitignored `sdkconfig`.
4. Status / last will: `mower/<name>/status` (retained `online` / `offline`), the only
   status topic the ACL lets a device login write.
5. Remote commands from the PC use the existing `app` login (no new VPS user).
6. **Identity in NVS** ([#36](https://github.com/rmx-lt/rmx-mower-gateway/issues/36)): the device name
   (`creds/device`, default `probe-device`) sets every login and topic, so one image fits every board.
   A second identity needs its own Mosquitto user + ACL on the VPS and its own web user; two boards on
   one login kick each other off (client id = username).

Open questions: modem firmware update B04 → B06 ([#20](https://github.com/rmx-lt/rmx-mower-gateway/issues/20)); whether the probe should
exercise the real `mower/mower-01/…` topics ([#21](https://github.com/rmx-lt/rmx-mower-gateway/issues/21)).

## Risks / gotchas to remember

- esp_modem default pins hit FLIGHT (25) and our TX (27) — always override.
- ESP and modem reset independently; PWRKEY pulse on a running modem turns it off.
- TLS handshakes cost ~4.3 s each over LTE: reuse connections.
- The default 1 MB app partition is nearly full with this stack (808 KB).
- CGNAT: no inbound connections; MQTT keepalive must be shorter than the carrier's NAT
  idle timeout (often minutes) — 60 s is a safe start.
- Let's Encrypt chain changes: keep ISRG Root X1 + X2.
- Mosquitto silently ignores forbidden SUBSCRIBE/PUBLISH: if nothing arrives, check the ACL first.
- SIM must be in before power-up; antenna on MAIN (CSQ 99 = no antenna).

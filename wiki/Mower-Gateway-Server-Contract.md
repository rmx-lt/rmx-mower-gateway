# Mower-Gateway-Server-Contract: What the device talks to: rmx-server / rmx-server-app

Sources: [Home](Home) (architecture), `blade-of-grass/CLAUDE.md`,
`rmx-server-app` README and `internal/protocol` (wire protocol),
`rmx-server-infra` `services/mosquitto/config/mosquitto.conf`, `services/mosquitto/config/acl`, `services/caddy/Caddyfile`.
Re-read those before coding: they are the source of truth and may have changed.

## Endpoints

| Endpoint | What | TLS |
|---|---|---|
| `https://app.rmx.lt/…` | rmx-server-app REST (Go behind Caddy) | Caddy, Let's Encrypt, TLS 1.2/1.3, HTTP/1.1 + h2 |
| `mqtts://mqtt.rmx.lt:8883` | Mosquitto 2.1, MQTT 3.1.1/5 over TLS (min TLS 1.2) | LE cert copied from Caddy |

Browsers do not speak MQTT. `https://mqtt.rmx.lt` only tells clients to use `mqtts://mqtt.rmx.lt:8883`.

- VPS `45.81.252.43` (`app`, `mqtt` are CNAMEs to `rmx.lt`). IPv4 only.
- rmx-server-app runs always on the VPS (since 2026-11-14; before that the probe at `/probe/…`, off by default).
  `GET /health` → `200 {"status":"ok","db":true,"mqtt":true,"version":"0.1.0"}` (answer checked 2026-11-14,
  before the move to `app.rmx.lt` on 2026-11-15).
- Mosquitto on 8883 runs always.

## TLS certificate chain (checked 2026-10-19 with openssl)

Both hosts serve Let's Encrypt's new **Gen-Y ECDSA** chain. The HTTPS host checked that day
was `api.rmx.lt` (removed 2026-11-15); `app.rmx.lt` gets its own leaf from the same issuer:

```
CN=<HTTPS host> / CN=mqtt.rmx.lt   ECDSA P-256 leaf, valid 2026-10-15 … 2027-01-13 (90 days, auto-renewed)
  └ CN=YE2 / CN=YE1 (Let's Encrypt) ECDSA P-384, to 2028-09-02
    └ CN=Root YE (ISRG)             ECDSA P-384, cross-signed by
      └ ISRG Root X2                ECDSA P-384, cross-signed by
        └ ISRG Root X1              RSA 4096 (self-signed root)
```

- Trust **ISRG Root X1** and **ISRG Root X2** (both are in ESP-IDF's cert bundle
  `cacrt_all.pem`; Root YE is not, which is fine because the server sends the cross-signs).
  Copies: [`certs/isrg-root-x1.pem`](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/certs/isrg-root-x1.pem),
  [`certs/isrg-root-x2.pem`](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/certs/isrg-root-x2.pem),
  [`certs/rmx-ca-bundle.pem`](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/certs/rmx-ca-bundle.pem) (both). openssl verifies both hosts with either root alone.
- Verified on the device: mbedTLS + `esp_crt_bundle_attach` (PPP path) and the modem's
  TLS with `isrgx1.pem` (AT path) both validate the chain.
- Since 0.2.3 the firmware's bundle is **only** `certs/rmx-ca-bundle.pem`
  (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_NONE` + custom bundle), not the full Mozilla
  `cacrt_all.pem`. Why: in Oct 2026 eight eMudhra/emSign CA keys were found to be trivially
  weak, and `emSign ECC Root CA - G3` is in ESP-IDF's full bundle, so anyone could have issued
  a `mqtt.rmx.lt` certificate the device accepted. If rmx.lt ever moves off Let's Encrypt,
  update the bundle and ship it over OTA **before** switching the server.
- Don't pin the leaf or the intermediate (they rotate every ~60–90 days). Pin roots only.
- Let's Encrypt chains change (they did in 2025–26); keep both roots, re-check with
  `openssl s_client -connect mqtt.rmx.lt:8883 -servername mqtt.rmx.lt -showcerts`.
- **SNI is required** for `app.rmx.lt` (Caddy serves several names on one IP). mbedTLS sends
  it automatically; the modem needs `AT+CSSLCFG="enableSNI",0,1`.

## Client certificate listener (port 8884)

Live since 2026-11-11 (see [rmx-server-infra#67](https://github.com/rmx-lt/rmx-server-infra/issues/67)), CA in wiki
Device-CA. The board logged in here with certificate serial 01, CN `mower-01`. What the firmware uses: `mqtts://mqtt.rmx.lt:8884`, the same server certificate as 8883
(checked against the ISRG bundle), and a client certificate chain (device certificate + issuing CA
certificate, ECDSA P-256) whose CN is the device name. The device sends no password. Identity =
CN (`use_identity_as_username`), so the ACL `mower/%u/#` applies unchanged. The firmware has no
password login any more (removed 2026-11-11); 8883 with passwords stays for the app and the PC tools.

Checked off-board 2026-11-10 against a throwaway Mosquitto 2.1.2 in Docker (`require_certificate true`,
cafile = throwaway root, CRL, `use_identity_as_username`): the chain file from `device-ca.ps1 sign` with a
P-256 key connects; the device certificate alone is refused ("unknown ca"), so the chain must be sent
(the firmware sends it); no certificate is refused. The board itself has not connected to 8884 yet.

## MQTT broker rules (Mosquitto, `mosquitto.conf` + `acl`)

- `allow_anonymous false`: every connection needs a login (password on 8883, client certificate on 8884).
- Listener 8883: TLS (`tls_version tlsv1.2` = minimum), **`use_username_as_clientid true`** —
  the broker replaces the client ID with the username. Topic schemes keyed on the client ID
  must use the username.
- ACL:
  - `pattern readwrite mower/%u/#` — **every login** may use only `mower/<its username>/#`.
  - `user app` → `mower/#` (backend).
  - `pattern read mower/events/#` and `pattern read mower/status/server` — every login may read the shared server topics.
  - `user probe-app` → `mower/#` (rmx-server-app).
- Password logins (8883): `app`, `probe-app`, `mower-test` (ACL tests). Passwords live only on the
  server (`services/mosquitto/data/credentials.txt`, root 0600). **Never print or commit them.**
  Devices have no password: `mower-01` exists only as a certificate CN on 8884 (its password login
  was deleted on 2026-11-13, https://github.com/rmx-lt/rmx-server-infra/issues/79).
- Mosquitto accepts a SUBSCRIBE to a forbidden topic but never delivers to it (no error!).
  A PUBLISH to a forbidden topic is silently dropped (MQTT 3.1.1).
- Bad login → CONNACK 5 "not authorized" (verified from the device).

### Mower topics: the contract

The contract between the mower and the server is wiki [Mower-Protocol](Mower-Protocol): every topic names its
source (`mower/<name>/gateway/…`, `mower/<name>/autopilot/…`), the gateway relays the autopilot's messages without
reading them, and it runs the NTRIP client and the link to the autopilot board. The firmware implements the
**gateway's own side** (§3 to §6, §12, §15, §16) since [issue 61](https://github.com/rmx-lt/rmx-mower-gateway/issues/61)
(first released in the firmware after 0.3.0); the NTRIP client and the autopilot link are separate issues
([blade-of-grass#6](https://github.com/rmx-lt/blade-of-grass/issues/6)). The unprefixed topics of older firmware
(`mower/<name>/status`, `cmd`, `cmd/ack`, `firmware`, `fw/…`) are gone; the first boot of the new firmware clears the
retained `status` and `firmware` messages that the old one left on the broker (flag `legacy` in NVS).

## What the gateway uses on MQTT (prefix `mower`)

| Topic | Direction | QoS | Retained | Payload |
|---|---|---|---|---|
| `mower/status/server` | server → all | 1 | yes | `online` / `offline` (server's LWT); the device only reads it |
| `mower/<name>/gateway/status` | device → server | 1 | yes | `online` / `offline` (also the last will) |
| `mower/<name>/gateway/state` | device → server | 1 | yes | `{"protocol","boot","firmware","previous","partition","autopilot_link","ntrip","operator","time"}` |
| `mower/<name>/gateway/telemetry` | device → server | 0 | no | `{"time","boot","metrics":{…}}`, every 60 s |
| `mower/<name>/gateway/metrics/index`, `…/metrics/<part>` | device → server | 1 | yes | metric catalogue (parts `signal`, `usage`, `system`) |
| `mower/<name>/gateway/monitor` | device → server | 0 | no | monitor samples (on request) |
| `mower/<name>/gateway/cmd` | server/PC → device | 1 | no | command (JSON) |
| `mower/<name>/gateway/cmd/ack` | device → server/PC | 1 | no | ack (JSON) |
| `mower/<name>/gateway/fw/req` | device → server | 1 | no | chunk request (JSON) |
| `mower/<name>/gateway/fw/chunk` | server → device | 1 | no | one firmware chunk (binary) |
| `mower/<name>/gateway/fw/res` | server → device | 1 | no | JSON error for a request |

- `<name>` is the certificate CN (1–32 chars `[A-Za-z0-9_-]`); the broker forces client id = username. `status` is
  reserved (the shared `mower/status/server`). The device subscribes `gateway/cmd`, `gateway/fw/chunk`,
  `gateway/fw/res` and `mower/status/server` on every connect (the shared one is covered by
  `pattern read mower/status/server`, the others by `pattern readwrite mower/%u/#`).
- Connection: **clean session**, **keep-alive 15 s** (verified on the board, see [Mower-Gateway](Mower-Gateway)).
- **Acks produced offline still arrive** ([#88](https://github.com/rmx-lt/rmx-mower-gateway/issues/88), verified on the board 2026-11-20). The gateway's QoS 1 publishes wait in esp-mqtt's outbox
  across a reconnect for up to 30 min (`CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS=1800000`; the supervisor restarts after 30 min without the server link, and RAM is
  lost on a restart). The outbox is capped at 32 KB (`outbox.limit`). The server can therefore get a final ack minutes late, after the gateway's `status` goes `online` again.
  Test: the radio was switched off (FLIGHT low) for 90 s, the `ping` ack was produced 4 s after MQTT dropped, and it arrived right after the reconnect 112 s later.
  The old 30 s expiry dropped such an ack (2026-11-19). An ack lost to a restart is still in the repeat memory only until that restart, so the server should keep resending
  an open command id (same id) until it gets a final ack or the command expires.
- **Server link** (Mower-Protocol §4.2): MQTT is up **and** the retained `mower/status/server` is `online`. When it
  comes up (also after boot) the gateway publishes `gateway/state` and the catalogue again; monitor streams end when
  it has been down 10 s (`main/mqtt_link.c`, `main/metrics.c`).
- The `autopilot/…` topics are not used yet (no board link).
- What the server serves today is in wiki [Server-App](Server-App); the contract both sides move to is
  [Mower-Protocol](Mower-Protocol). The server's HTTP and WebSocket parts are not used by the gateway.

### Commands and acks

`ping`, `restart`, `ota`, `monitor_start`, `monitor_stop` (Mower-Protocol §5, §6.4, §15). `ntrip_set` / `ntrip_clear`
answer `unknown_cmd` and `autopilot_reset` answers `failed` ("line not wired"): those belong to the NTRIP and
autopilot issues. The `status` and `gps` commands are gone (the modem's GNSS is not used; `main/gnss.c` was removed).

- Ack: `{"id","cmd","ok","state":"accepted|done|failed","result"?,"error"?,"message"?,"time"}`; `time` is a number or
  `null` while the clock is not set. Every command gets one final ack; `ota` first gets `accepted`.
- **Repeats:** the gateway remembers the last 16 command ids for 3 minutes with their last ack (`main/remote_cmd.c`).
  The same id again is not run again: the last ack is sent again (`accepted` while the command still runs).
- Errors: `bad_request`, `unknown_cmd`, `expired`, `no_clock`, `busy` (an update is running; `monitor_*` still work),
  `failed`, `over_budget` (monitor), and for `ota` `same_version`, `download_failed`, `bad_image`, `bad_size`,
  `low_signal`, `rolled_back`.
- `ping` result `{"pong":"<name>"}`. `restart` acks `done`, then restarts after 2 s.

### Metrics, telemetry, data usage, monitor

- Catalogue (`main/metrics.c`, revision 1): `signal.rssi` (key `signal.strength`, dBm), `usage.total.up|down`,
  `usage.mqtt.up.*` / `usage.mqtt.down.*` (family: one counter per topic), `system.cpu`, `system.core0|1`,
  `system.memory.free|min_free|largest|pct`, `system.nvs.pct`. The NTRIP (`usage.ntrip.*`) and board-link
  (`link.*`) metrics join when those exist.
- `signal.rssi` is read once a minute with PPP paused for 2 to 3 s (the modem answers AT only in command mode), so it
  offers no monitor rates; every other metric offers 1, 2, 5, 10 samples per second except `system.nvs.pct`.
- `usage.total.*` count IP packets on the PPP interface (output hook on the lwIP netif, `--wrap=ip4_input` for the
  receive side, `main/usage.c`); `usage.mqtt.*.<topic>` count whole MQTT PUBLISH packets (fixed header, topic, packet
  id, payload) per topic, named without `mower/<name>/`.
- Monitor (`monitor_start` / `monitor_stop`, `gateway/monitor`): budget 100 values per second as the only limit
  (a start that does not fit is `over_budget`; no cap on the number of streams, each costs one allocation), a metric can be listed once per stream, hold 1 to 60 s (default 10), samples sent every
  0.2 s (every sample at rates 1, 2, 5), streams end when the server link is lost.

## Firmware over MQTT

The device downloads a signed image as binary chunks on the MQTT connection it already
has. The file is `<version>.bin` in the server's firmware folder; there is no HTTPS download
(Caddy's `/fw/` was removed on 2026-11-13). The contract is [Mower-Protocol](Mower-Protocol) §6.5 and §12; the byte layout
the server uses is in wiki [Server-App](Server-App#firmware-over-mqtt). This is what the gateway does (`main/ota.c`):

1. The `ota` command carries `version`, optional `file` (default `<version>.bin`) and optional `size`. The gateway
   checks it (`bad_request`, `same_version`, `busy`, `low_signal`), acks `accepted` and downloads.
2. One request at a time on `mower/<name>/gateway/fw/req`: `{"request_id","file","chunk"}`, QoS 1, a **new
   request id for every request**.
3. It subscribes `gateway/fw/chunk` and `gateway/fw/res` on every connect.
   A chunk is a 64-byte little-endian header (`RMFW`, version 1, index, count, image size,
   chunk length, request-id hash, whole-image SHA-256) plus up to 4096 bytes. Errors are JSON on `fw/res`.
4. esp-mqtt `buffer.size` is **8192**, so a chunk plus the 64-byte header
   arrives as one `MQTT_EVENT_DATA`. `on_data` still reassembles a frame that is split
   (`current_data_offset` / `total_data_len`), up to 128 KB.
5. The device rejects a frame whose magic, version, flags, header length, reserved field or
   length do not match, whose chunk is longer than 4096 bytes, or whose request-id hash is not the first 8 bytes of
   SHA-256 over the request id it sent. It writes the payload with `esp_ota_begin` / `esp_ota_write`.
6. After the app-description bytes are in the slot it checks project name, version (must
   equal the command) and that the version is not the one already running. `size` on the
   command, when set, must equal the image size in the header.
7. It hashes every payload byte and compares the digest to the header SHA-256. Then
   `esp_ota_end` checks the image, which includes the RSA-3072 signature
   (`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`). Only then
   `esp_ota_set_boot_partition`.
8. **Timeout and retries:** the gateway waits **10 s** for an answer, then asks for the same chunk again with a new
   request id, up to 5 times in a row. After that, or on a JSON error answer (`not_found`, `bad_request`, `internal`),
   the update ends with the final ack `download_failed`. Bytes already written stay, so a retry resumes at that chunk.
   A board reset loses the progress; the next `ota` command starts at chunk 0. The running image is unchanged until
   step 7 succeeds.
9. **After the restart:** before it restarts the gateway stores the command id, the target version and the old version
   in NVS (`ota_id`, `ota_ver`, `ota_prev`). The new image confirms itself once MQTT is up (rollback timer
   `CONFIG_GATEWAY_OTA_CONFIRM_S`), and when the server link is up it sends the final ack `done` with
   `{"version","previous","partition"}`; `previous` is also kept in NVS and shows in `gateway/state`. If the new image
   does not confirm, the old one boots again, sees the stored id and sends the final ack `failed` / `rolled_back`.
   A resend of the id during that time gets `accepted`.
10. When `FIRMWARE_DIR` is unset the server does not subscribe to `fw/req`, so a request gets
    no answer (`download_failed`).

### Verified on the board (2026-11-15, issue 61 build, version string 0.3.0)

Over LTE with the client certificate on 8884, read back from the broker with `tools/mqtt_watch.ps1`:
`gateway/status` `online`; `gateway/state` (retained) with protocol `0.1`, boot, firmware, `previous` null, partition and
the operator code; the catalogue (`metrics/signal`, `usage`, `system` of 372, 1313 and 1938 bytes, plus `metrics/index`,
revision 1); `gateway/telemetry` every 60 s with the signal, `usage.total.*`, the `system.*` values and one
`usage.mqtt.up.<topic>` / `usage.mqtt.down.<topic>` counter per topic that carried data. `ping` is acked `done`; the same
id sent again returns the same ack (same `time`). `restart` acks `done` and the board is back in under 45 s with `boot` one higher. An unknown command is `unknown_cmd`, a past `expires_at` is `expired`.
`monitor_start` streams samples (2 per message at 10/s, 1 at 5/s), `used` in the ack adds up (15, then 95 of 100), a
stream that does not fit is `over_budget`, the same session with other metrics is `bad_request`, `monitor_stop` ends all.
`ota` of a version nobody serves: `accepted` at once, `busy` for other commands, the same id repeats `accepted`, and after
6 requests 10 s apart the final ack `download_failed` (about 60 s).

Not verified: a complete update (no image was served on the new topics), `rolled_back`, the final ack after the restart,
monitor streams ending after a server outage.

Note for tests: `tools/mqtt_cmd.ps1` and `tools/mqtt_watch.ps1` both log in as `app`, and the broker forces client id
`app`, so starting one disconnects the other for about 1.5 s (gaps in a watched monitor stream come from that).

### HTTPS

Browsers use the server's HTTP API (wiki [Server-App](Server-App), at `https://app.rmx.lt/`). The gateway
firmware does not. `tools/ppp_smoke` still does one GET `https://app.rmx.lt/health` as a pipe check.
NVS holds no web or firmware-download password.

# Mower-Gateway-Server-Contract: What the device talks to: rmx-server / rmx-server-app

Sources: [Home](Home) (architecture), `blade-of-grass/CLAUDE.md`,
`rmx-server-app` README and `internal/protocol` (wire protocol),
`rmx-server-infra` `services/mosquitto/config/mosquitto.conf`, `services/mosquitto/config/acl`, `services/caddy/Caddyfile`.
Re-read those before coding: they are the source of truth and may have changed.

## Endpoints

| Endpoint | What | TLS |
|---|---|---|
| `https://api.rmx.lt/probe/…` | rmx-server-app REST (Go behind Caddy) | Caddy, Let's Encrypt, TLS 1.2/1.3, HTTP/1.1 + h2 |
| `mqtts://mqtt.rmx.lt:8883` | Mosquitto 2.1, MQTT 3.1.1/5 over TLS (min TLS 1.2) | LE cert copied from Caddy |

Browsers do not speak MQTT. `https://mqtt.rmx.lt` only tells clients to use `mqtts://mqtt.rmx.lt:8883`.

- VPS `45.81.252.43` (`api`, `mqtt` are CNAMEs to `rmx.lt`). IPv4 only.
- **The probe is off by default on the VPS** (owner starts it when needed). Off → `/probe/*`
  returns 502. On 2026-10-19 it was on: `GET /probe/health` → `200 {"status":"ok","db":true,"mqtt":true,"version":"0.1.0"}`
  (one earlier request that day got 401 — probably during a redeploy).
- Mosquitto on 8883 runs always.

## TLS certificate chain (checked 2026-10-19 with openssl)

Both hosts serve Let's Encrypt's new **Gen-Y ECDSA** chain:

```
CN=api.rmx.lt / CN=mqtt.rmx.lt     ECDSA P-256 leaf, valid 2026-10-15 … 2027-01-13 (90 days, auto-renewed)
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
- **SNI is required** for `api.rmx.lt` (Caddy serves several names on one IP). mbedTLS sends
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

- `allow_anonymous false`: every connection needs username + password.
- Listener 8883: TLS (`tls_version tlsv1.2` = minimum), **`use_username_as_clientid true`** —
  the broker replaces the client ID with the username. Topic schemes keyed on the client ID
  must use the username.
- ACL:
  - `pattern readwrite mower/%u/#` — **every login** may use only `mower/<its username>/#`.
  - `user app` → `mower/#` (backend).
  - `pattern read mower/events/#` and `pattern read mower/status/server` — every login may read the shared server topics.
  - `user probe-app` → `mower/#` (probe server; plus legacy `probe/#` until the switch is done).
  - `user probe-device` → legacy, to be removed: write `probe/req/probe-device`,
    read `probe/res/probe-device`, `probe/events/#`, `probe/status/#`.
- Logins that exist: `app`, `mower-01`, `probe-app`, `probe-device` (legacy). Passwords live only on the
  server (`services/mosquitto/data/credentials.txt`, root 0600). **Never print or commit them.**
  The `mower-01` password is no longer used by the device (it logs in with its certificate); retiring that
  server-side login is a server task.
- Mosquitto accepts a SUBSCRIBE to a forbidden topic but never delivers to it (no error!).
  A PUBLISH to a forbidden topic is silently dropped (MQTT 3.1.1).
- Bad login → CONNACK 5 "not authorized" (verified from the device).

### Planned mower topics (architecture, not implemented by any server yet)

| Topic | Direction | Notes |
|---|---|---|
| `mower/<id>/telemetry` | mower → server | |
| `mower/<id>/status` | mower → server, retained | also the Last Will (`offline`) |
| `mower/<id>/cmd` | server → mower | command with ID and expiry |
| `mower/<id>/cmd/ack` | mower → server | |

Safety: the mower stops on its own when the link or acks are lost; never rely on the server.

The gateway already uses `mower/<name>/cmd` → `…/cmd/ack` and `…/status`
(format in [06](Mower-Gateway-Plan), "Remote commands").

## rmx-server-app protocol (the probe target)

The probe server is a CRUD demo on a `records(time, connection_type, value)` table, over
HTTPS REST, WSS and MQTT. Every change is pushed to all clients.

### MQTT (topic prefix `mower`; moved from `probe/` by issue 53, 2026-11, hardware test pending)

| Topic | Direction | Payload |
|---|---|---|
| `mower/<client-id>/req` | client → server | request envelope (JSON) |
| `mower/<client-id>/res` | server → client | response envelope |
| `mower/events/records` | server → all | change event |
| `mower/status/server` | server → all, **retained** | `online` / `offline` (server's LWT) |
| `mower/<client-id>/fw/req` | client → server | firmware request (JSON) |
| `mower/<client-id>/fw/chunk` | server → client | one firmware chunk (binary) |
| `mower/<client-id>/fw/res` | server → client | firmware info, or a JSON error |

- The server subscribes `mower/+/req` (QoS 1), answers on `mower/<same id>/res` (QoS 1).
  Client id: 1–64 chars `[A-Za-z0-9_-]`; `events` and `status` are reserved (shared topics).
- **Which login can a device use?** Its own, e.g. `mower-01`. On 8883 the client id is forced to the username, so the device publishes on `mower/mower-01/req` and subscribes `mower/mower-01/res`, `mower/events/records`, `mower/status/server` (the first two covered by `pattern readwrite mower/%u/#`, the shared ones by the `pattern read` rules). The old `probe-device` login only reached `probe/…` topics. `probe-app` is the server's login, not a device's.
- Requests (`internal/protocol` in rmx-server-app):

```jsonc
{ "request_id": "<any string, e.g. uuid>", "request": { "op": "list" } }
{ "request_id": "r2", "request": { "op": "create", "value": "hello from mower" } }
{ "request_id": "r3", "request": { "op": "update", "id": 7, "value": null } }
{ "request_id": "r4", "request": { "op": "delete", "id": 7 } }
```

- Responses:

```jsonc
{ "request_id": "r2", "result": { "status": "ok", "data": { "kind": "record", "record": {
    "id": 7, "time": "2026-10-18T12:00:00Z", "connection_type": "mqtt", "value": "hi",
    "updated_at": "2026-10-18T12:00:00Z" } } } }
{ "request_id": "r1", "result": { "status": "ok", "data": { "kind": "records", "records": [ … up to 500 ] } } }
{ "request_id": "r4", "result": { "status": "ok", "data": { "kind": "deleted", "id": 7 } } }
{ "request_id": "x",  "result": { "status": "error", "error": { "code": "not_found", "message": "…" } } }
```

  Error codes: `bad_request`, `not_found`, `internal`, `unauthorized`, `forbidden`, `too_many_requests`.
  `value` ≤ 1000 characters (or null). A `list` response can be large (≤ 500 records):
  set the MQTT client's `buffer.size` big enough (e.g. 64 KB, in PSRAM) or avoid `list`.
- Events: `{"op":"create","record":{…}}`, `{"op":"update","record":{…}}`,
  `{"op":"delete","id":7,"connection_type":"mqtt"}`.

## Firmware over MQTT

The device downloads a signed `*.bin` as binary chunks on the MQTT connection it already
has. The file name is the same last segment Caddy serves
(`https://api.rmx.lt/fw/probe/<version>.bin` → file `<version>.bin`). The device does not
open that URL. The byte layout, request JSON, error codes and chunk size are the contract
in wiki [Server-App](Server-App#firmware-over-mqtt). This is what the gateway does with it:

1. The `ota` command carries `version` and optional `file` (default `<version>.bin`) and
   optional `size`. Code: `main/ota.c`, command handling in `main/remote_cmd.c`.
2. The device publishes one JSON request at a time on `mower/<name>/fw/req`
   (`request_id`, `op` `chunk`, `file`, `chunk`). QoS 1, not retained.
3. It subscribes to `mower/<name>/fw/chunk` and `mower/<name>/fw/res` on every connect.
   A chunk is a 64-byte little-endian header (`RMFW`, version 1, index, count, image size,
   chunk length, request-id hash, whole-image SHA-256) plus the raw bytes. Info and errors
   are JSON on `fw/res`.
4. esp-mqtt `buffer.size` is **8192**, so a default 4096-byte chunk plus the 64-byte header
   arrives as one `MQTT_EVENT_DATA`. `on_data` still reassembles a frame that is split
   (`current_data_offset` / `total_data_len`), up to 128 KB.
5. The device rejects a frame whose magic, version, flags, header length, reserved field or
   length do not match, or whose request-id hash is not the first 8 bytes of SHA-256 over
   the request id it sent. It writes the payload with `esp_ota_begin` / `esp_ota_write`.
6. After the app-description bytes are in the slot it checks project name, version (must
   equal the command) and that the version is not the one already running. `size` on the
   command, when set, must equal the image size in the header.
7. It hashes every payload byte and compares the digest to the header SHA-256. Then
   `esp_ota_end` checks the image, which includes the RSA-3072 signature
   (`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`). Only then
   `esp_ota_set_boot_partition`.
8. A chunk timeout retries that same chunk (same request id, 4 tries, 45 s each). Bytes
   already written stay, so the next index resumes the download. A board reset loses that
   progress; the next `ota` command starts at chunk 0. The running image is unchanged until
   step 7 succeeds.
9. When `FIRMWARE_DIR` is unset the server does not subscribe to `fw/req`, so a request gets
   no answer (`download_failed`). A JSON error `firmware is not configured` is the same fact
   if a request is still dispatched.

### HTTPS

Browsers still use `https://api.rmx.lt/probe/…` (wiki [Server-App](Server-App)). The gateway
firmware does not. `tools/ppp_smoke` still does one GET `/probe/health` as a pipe check.
NVS holds no web or firmware-download password.

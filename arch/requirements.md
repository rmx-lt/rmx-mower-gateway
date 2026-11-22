# Requirements

What the rmx-mower-gateway firmware must do. The gateway is the mower's communications board (LilyGo T-SIM7600:
ESP32 + SIM7600G-H): it links the autopilot board to rmx-server over LTE, relays RTK corrections from an NTRIP
caster, and updates its own firmware over the air. Architecture decisions cite these requirements by ID.

A requirement says **what** is needed and **why**, not how it is built: values the firmware tunes (timeouts, key
types) belong to the decisions. Each requirement names its source: a page of the Blade of Grass wiki
(https://github.com/rmx-lt/blade-of-grass/wiki, where Mower-Protocol is the contract with the server and the
autopilot), a file in this repository, or the owner.

Requirements are **functional** (what the gateway does) or **non-functional** (how well it does it, and the limits
it fits in), and each kind is sorted into areas. Functional areas use IDs below 100, non-functional ones 100 and up;
each area has its own range in tens, so a gap is room for a new requirement. Once decisions cite an ID, it never
changes.

## Kinds

| Kind | Description |
|---|---|
| Functional | What the gateway does: its behaviour on each link and the services it offers. |
| Non-functional | How well the gateway must do it, and the limits it must fit in. |

## Areas

| Area | Kind | IDs | Description |
|---|---|---|---|
| [Cellular link and time](#cellular-link-and-time) | Functional | REQ-001–009 | The LTE data connection through the modem, the clock set from the network, and the LED that shows the link. |
| [Server protocol](#server-protocol) | Functional | REQ-010–019 | The gateway's side of the MQTT contract with the server: topics, server link, state. |
| [Commands](#commands) | Functional | REQ-020–029 | Commands from the server and their acknowledgements. |
| [Firmware update](#firmware-update) | Functional | REQ-030–039 | Downloading, checking and installing a new firmware image. |
| [NTRIP corrections](#ntrip-corrections) | Functional | REQ-040–049 | The caster session that feeds RTK corrections to the autopilot. |
| [Board link to the autopilot](#board-link-to-the-autopilot) | Functional | REQ-050–059 | The UART link that relays the autopilot's messages and carries corrections and heartbeats. |
| [Metrics, telemetry, monitor](#metrics-telemetry-monitor) | Functional | REQ-060–069 | What the gateway measures and how it reports it. |
| [Identity and provisioning](#identity-and-provisioning) | Functional | REQ-070–079 | The board's certificate identity and how it gets one. |
| [Local link to a phone](#local-link-to-a-phone) | Functional | REQ-080–089 | A phone near the mower talking to the gateway directly over Wi-Fi. |
| [Reliability and recovery](#reliability-and-recovery) | Non-functional | REQ-100–109 | Staying in service and recovering from faults without a person on site. |
| [Security](#security) | Non-functional | REQ-110–119 | Keeping secrets secret and trusting only the right server. |
| [Platform and resources](#platform-and-resources) | Non-functional | REQ-120–129 | The board, the modem's bandwidth and the flash the firmware must fit. |
| [Testability](#testability) | Non-functional | REQ-130–139 | Checking the firmware's logic without the board. |

## Functional

### Cellular link and time

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-001 | Reach the server over LTE, MQTT over TLS only | The gateway reaches rmx-server through the SIM7600G-H modem's data connection (PPP) and talks to it only with MQTT over TLS (`mqtts://mqtt.rmx.lt:8884`). It has no HTTPS client; firmware arrives over MQTT too. | `CLAUDE.md` (goal); Mower-Gateway (intro); Mower-Protocol §1 (gateway role: communications only). |
| REQ-002 | Set the clock from network time before TLS | The gateway sets its clock with SNTP after the data connection is up and keeps it within 0.1 s. It opens no TLS connection before the clock is set, because TLS checks certificate dates; when no time arrives, it does not wait forever but starts over. Command expiry (`expires_at`) and the autopilot's clock depend on it. | Mower-Protocol §4.1; Mower-Gateway-Plan (certificate dates); `main/app_main.c`. |
| REQ-003 | Ask the modem while online without losing the connection | The gateway reads the signal from the modem once a minute while the data connection runs. A reading may pause data for 2–3 s, but must not end the PPP session. | Mower-Gateway-PPP ("AT commands while online"); Mower-Protocol §16.5 (`signal.rssi` notes). |
| REQ-004 | Control the modem's power and radio | The modem is powered with PWRKEY (a pulse toggles power, so the gateway checks whether the modem already runs before pulsing), its radio enabled with FLIGHT high, kept awake with DTR low, and its state read from STATUS. | Mower-Gateway-Board-Hardware (pins); `CLAUDE.md` (pitfalls). |
| REQ-005 | Show the data link on the user LED | The blue user LED is on while the cellular data connection (PPP) is up. | As built (`main/cell.c`); confirmed by the owner (2026-11-22). |

### Server protocol

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-010 | Speak the gateway side of Mower-Protocol 0.1 | The gateway publishes and subscribes the topics of Mower-Protocol §3 under `mower/<name>/gateway/…`, connects with a clean session and a 15 s keep-alive, and keeps a retained `gateway/status` (`online` after every connect, `offline` as the last will). | Mower-Protocol §3, §6.1. |
| REQ-011 | Act on the server link as the contract defines it | The server link is up when MQTT is connected and the retained `mower/status/server` says `online`; it is lost after 10 s down without a break. When it comes up, also the first time after boot, the gateway publishes its state and metric catalogue again. | Mower-Protocol §4.2. |
| REQ-012 | Report the gateway's state | A retained `gateway/state` with `protocol`, `boot`, `firmware`, `previous`, `partition`, `autopilot_link`, `ntrip`, `operator` and `time`, published when the server link comes up and whenever a field changes. Each field reflects the part of the firmware that owns that fact. | Mower-Protocol §6.2. |
| REQ-013 | Count boots across restarts | A boot counter that grows by one at every start, kept in flash, is in state, telemetry and events. | Mower-Protocol §4.1. |
| REQ-014 | Handle the contract's message sizes | JSON messages are at most 4096 bytes. Chunk messages (4096 bytes plus a 64-byte header) arrive in MQTT fragments, which the gateway puts back together before use. | Mower-Protocol §4, §12.2; `CLAUDE.md` (pitfalls: chunks, MQTT receive buffer). |

### Commands

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-020 | Answer every command exactly once, also when repeated | Commands use the envelope of Mower-Protocol §5: exactly one final ack (`done` or `failed`), an `accepted` ack first for long-running commands, `expired` / `no_clock` for `expires_at`, `bad_request` and `unknown_cmd`. The gateway remembers each command id with its last ack for at least 2 minutes and sends that ack again for a repeat instead of running the command twice. | Mower-Protocol §5. |
| REQ-021 | Offer the gateway commands | `ping`, `restart` (ack, then restart), `ota`, `ntrip_set`, `ntrip_clear`, `autopilot_reset`, `monitor_start`, `monitor_stop`, with the parameters, results and errors of the contract. | Mower-Protocol §6.4. |
| REQ-022 | Refuse other commands during an update | While a firmware update runs, every command except `monitor_start` and `monitor_stop` is answered `busy`. | Mower-Protocol §6.5. |

### Firmware update

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-030 | Download firmware as MQTT chunks | An update pulls the image over `gateway/fw/req\|chunk\|res`: one request open, 10 s wait, the same chunk asked again with a new request id up to 5 times, then `download_failed`. Chunks whose header does not match the open request are dropped. The running firmware is untouched until the whole image checks out. | Mower-Protocol §12; Mower-Gateway (Firmware updates, retries). |
| REQ-031 | Install only a valid, signed image | Before switching, the gateway checks the image's signature (signed with the project's own key), project name, version (`same_version`), size when given (`bad_size`) and the SHA-256 of the whole file (`bad_image`). It refuses to start an update when the signal is too weak (`low_signal`). | Mower-Protocol §6.5; Mower-Gateway (Firmware updates). |
| REQ-032 | Keep a new image only when it reaches the server | A new image confirms itself only when it reaches the broker within a limited time; otherwise the previous image boots again. The command id survives the restart, and the image that runs afterwards sends the final ack: `done` with `{version, previous, partition}`, or `failed` with `rolled_back`. | Mower-Protocol §6.5; Mower-Gateway (Firmware updates, report). |

### NTRIP corrections

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-040 | Store the caster settings on the gateway | Host, port, mount point, user and password are stored in flash, set by `ntrip_set` (reconnect at once) and removed by `ntrip_clear`. They survive restarts and updates. | Mower-Protocol §6.4, §13; Mower-Gateway (NTRIP client). |
| REQ-041 | Run the caster session | The gateway forwards the autopilot's GGA sentences (one every 10 s) to the caster and stays `waiting` until a GGA with a position arrives. A silent caster, a closed socket or a lost data connection ends the session, and the gateway connects again by itself, waiting longer after repeated failures and after a refused login so it does not hammer the caster. The session state is reported in `gateway/state` (`ntrip`). | Mower-Protocol §6.2, §13; Mower-Gateway (NTRIP client). |
| REQ-042 | Pass corrections to the autopilot unchanged | The caster's bytes go to the autopilot in order and unchanged, split wherever convenient. | Mower-Protocol §13, §14.3. |

### Board link to the autopilot

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-050 | Frame the board link | UART at 921600 baud, 8N1, RTS/CTS. Frames are type, body and CRC-16 (CCITT-FALSE), COBS-encoded, ending in `0x00`, at most 4608 bytes before encoding. Bad or unknown frames are dropped and counted (`link.crc_errors`). | Mower-Protocol §14, §14.1. Planned: the link is not built yet. |
| REQ-051 | Relay the autopilot's MQTT traffic without reading it | The gateway publishes and subscribes `mower/<name>/autopilot/…` on the autopilot's behalf, adding the prefix on publish and removing it on delivery, so the autopilot can use only its own topics. It never reads or changes autopilot payloads. | Mower-Protocol §1 (gateway role), §3, §14.2. |
| REQ-052 | Deliver reliable frames on the board link | QoS 1 messages and SUBSCRIBE are numbered 1–255, one in flight per direction, acknowledged, resent after 200 ms up to 5 times, then dropped and counted (`link.dropped`). A repeat is acknowledged again and not passed on. | Mower-Protocol §14.2. |
| REQ-053 | Keep the autopilot's subscriptions | A SUBSCRIBE replaces the previous list; the gateway subscribes the list again after every MQTT reconnect and asks for it (heartbeat bit 4) from its start until it has one. | Mower-Protocol §14.2, §14.4. |
| REQ-054 | Hold reliable messages while MQTT is down | While MQTT is disconnected, reliable messages from the autopilot are kept for up to 60 s and 64 KB, oldest dropped and counted beyond either. Other messages are dropped at once; messages for the autopilot while the board link is down are dropped and counted. | Mower-Protocol §14.2. |
| REQ-055 | Exchange heartbeats | Every second the gateway sends a heartbeat with the link version, the flags (MQTT connected, server online, corrections streaming, clock valid, subscriptions needed) and the time. The link is down at start, up from the first heartbeat received, down after 3 s without one; an unknown link version keeps it down and reports `autopilot_link: "incompatible"`. | Mower-Protocol §6.2, §14.4. |
| REQ-056 | Reset the autopilot on command | `autopilot_reset` pulses the autopilot's reset line (open-drain, GPIO21 to the P4's CHIP_PU) and fails when the line is not wired. | Mower-Protocol §6.4; Mower-Hardware (wiring). |

### Metrics, telemetry, monitor

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-060 | Publish a metric catalogue | Every value the gateway measures is a metric in its catalogue, published retained in parts of at most 4096 bytes under `gateway/metrics/<part>` with an `index` and a `revision`, whenever the server link comes up. A metric answers a server key where the contract asks (`signal.rssi` → `signal.strength`). | Mower-Protocol §16, §16.5. |
| REQ-061 | Send telemetry at the catalogue's periods | `gateway/telemetry` carries `time`, `boot` and the metrics due, each at its catalogue period (60 s for most gateway metrics): signal, processor and memory load, flash use, link counters, data usage. | Mower-Protocol §6.3, §16.3. |
| REQ-062 | Count mobile data by use | Bytes since boot, up and down: in total on the network interface (what the SIM is charged for), on the caster connection, and per MQTT topic as whole packets. Only sizes are counted, never contents. | Mower-Protocol §6.3 (data usage). |
| REQ-063 | Stream values on demand | Monitor sessions as in Mower-Protocol §15: several streams side by side within a budget of 100 values per second, each ending `hold` seconds (at most 60) after its last `monitor_start`, on `monitor_stop`, or when the server link is lost; one message per stream every 0.2 s on `gateway/monitor`. | Mower-Protocol §15. |

### Identity and provisioning

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-070 | Identify by a client certificate only | The board makes its own ECDSA P-256 key on first boot; the key never leaves it. Its name is the CN of its certificate and nothing else, so one image fits every board. There are no passwords on the board. | Mower-Gateway (Identity: client certificate only); README. |
| REQ-071 | Be provisioned over the serial console | Without a valid certificate the board starts no modem and no MQTT and waits for provisioning. Over UART0 it answers `RMX-CSR <name>`, stores a chain (`RMX-CERT-BEGIN` … `RMX-CERT-END`) and clears it (`RMX-CERT-CLEAR`), touching only the chain, never the key. | Mower-Gateway (Identity); README. |

### Local link to a phone

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-080 | Let a phone drive the mower over local Wi-Fi | A phone near the mower connects to the gateway over Wi-Fi and sends the same autopilot commands and live-driving setpoints as the server. The transport and its protocol are not defined yet (not part of Mower-Protocol 0.1). | Mower-Protocol §1 (gateway role: "later a local link"), §9 (last paragraph); owner (2026-11-22). |

## Non-functional

### Reliability and recovery

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-100 | Restore the data connection by itself | When PPP drops, the gateway dials again; when redialling keeps failing, it power-cycles the modem. No person is near the mower to press a button. | Mower-Gateway-Architecture §4.3 (recovery ladder step 2); Mower-Gateway-PPP. |
| REQ-101 | Escalate when the link is half dead | When the data connection is up but the server cannot be reached, the gateway gets back on line by itself, trying the least disruptive step first and restarting the board only as a last resort. It does not act on short outages the network or the server recover from on their own. | Mower-Gateway-Architecture §4.3 (recovery ladder steps 1 and 3). |
| REQ-102 | Keep corrections independent of the server | The NTRIP client uses its own TCP connection to the caster over LTE; corrections never pass through MQTT or the server, so positioning keeps working while the server is down. | Mower-Protocol §13. |
| REQ-103 | Recover from a stuck task | A firmware task that stops making progress is detected and the board restarts, with the reason logged. A task that legitimately waits a long time (a redial) is not mistaken for a stuck one. | Mower-Gateway-Architecture §4.3. |
| REQ-104 | Never interrupt an image write | No automatic restart or recovery step runs while a firmware image is being written. | Mower-Gateway-Architecture §4.3 (ladder step 3). |
| REQ-105 | Keep a crash for later | The core dump of the last crash stays in flash until the next crash, readable over USB. | `partitions.csv`; `main/app_main.c`. |
| REQ-106 | Keep settings and identity across updates | USB flashing and OTA updates leave the key, certificate, caster settings and boot counter untouched. | README; Mower-Gateway (Identity). |

### Security

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-110 | Never log or send secrets | The caster password and the private key are never logged, never put in an ack and never reported back. A command that carries a secret is logged by name and id only, at every log level. | Mower-Protocol §5 ("Acks never carry secrets"), §13; Mower-Gateway (NTRIP client); Mower-Gateway-Architecture §1.5. |
| REQ-111 | Trust only the server's roots | TLS trusts only ISRG Root X1 and X2 and checks certificate dates. | Mower-Gateway (Identity, last paragraph); Mower-Gateway-Plan (risks: Let's Encrypt chain). |
| REQ-112 | Keep the public image free of secrets | The repository and firmware image hold no passwords, tokens or modem identifiers; secrets live only in the board's flash. | `CLAUDE.md` (rules). |

### Platform and resources

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-120 | Run on the T-SIM7600 board | ESP32-WROVER-E (16 MB flash, 8 MB PSRAM) with the SIM7600G-H on UART1 (TX 27, RX 26, no flow control), the board link on UART2 (TX 23, RX 19, RTS/CTS), the provisioning console on UART0 (CH9102). GPIO12 (user LED) is a strapping pin and must stay low at reset. | Mower-Gateway-Board-Hardware; Mower-Hardware (wiring); `CLAUDE.md` (pitfalls). |
| REQ-121 | Fit the modem's bandwidth | Everything shares the modem's serial link, about 11 KB/s. Corrections take about 1.1 KB/s, monitor streams are capped by their budget, and downloads run one at a time. | Mower-Protocol §12.4, §15; Mower-Gateway (NTRIP client, measured); Mower-Hardware. |
| REQ-122 | Fit two image slots | The image fits a 4 MB app slot, with a second slot for the update. | `partitions.csv`. |

### Testability

| ID | Title | Requirement | Source |
|---|---|---|---|
| REQ-130 | Test the pure logic on a PC | Protocol logic that needs no hardware (framing, CRC, reliable-frame state, queues, chunk headers, command id memory, NTRIP parsing, recovery decisions) runs in host tests in CI. | Mower-Gateway-Architecture §2 (finding 5), §1.5 (tests). |

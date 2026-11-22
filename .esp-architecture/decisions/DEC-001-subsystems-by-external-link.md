# DEC-001 Subsystems by external link

## Context

The firmware needs a few subsystems that each bundle components used together, so the whole system can be drawn and
reasoned about one level at a time. The gateway is a communications board: almost everything it does is moving data
over one of its links (LTE modem, server over MQTT, autopilot over UART, NTRIP caster, later a phone over Wi-Fi).

The as-built code groups modules into five sets: cellular, server link, autopilot link, app and a `platform` set.
`platform` is not a bundle: its settings store, task table, link state and pure-C codec have no links between them and
each serves a different set of users. The server-link set holds about eleven modules, from the MQTT client to OTA and
metrics.

## Decision

Seven subsystems, one per external link plus one that runs the board:

| Subsystem | Owns |
|---|---|
| `cellular` | Modem pins and power, PPP, signal reading, redial, the user LED that shows the data link |
| `server_link` | The MQTT client, the server-link rule, the certificate identity and its provisioning console |
| `gateway_services` | Commands, firmware update, gateway state, metrics, telemetry, monitor streams, data-usage counting |
| `autopilot_link` | The board-link UART, framing, reliable delivery, relaying, heartbeat, the autopilot reset line |
| `corrections` | The NTRIP caster session |
| `local_link` | The Wi-Fi link to a phone (planned) |
| `system` | Start-up order, recovery, task liveness, the settings store, boot counter, clock, link state |

Consequences:

- The shared pure-C codec is split by user: each subsystem keeps its own host-tested core (board-link framing in
  `autopilot_link`, firmware chunks and command handling in `gateway_services`, NTRIP parsing in `corrections`,
  recovery logic in `system`).
- The settings store, task table and link state stay together in `system`: every subsystem uses them, and flash
  storage needs one owner.
- Each link counts its own bytes; data-usage counting in `gateway_services` reads those counts from below, so no link
  calls up into services.
- `corrections` is its own subsystem so that it depends on the cellular link and the autopilot link, never on the
  server link.

## Alternatives

- **Keep the five as-built sets.** Least code movement, but `platform` stays a set of unrelated helpers and the server
  link stays the one box everything touches.
- **One subsystem for the whole firmware.** Every project has subsystems, and one subsystem makes the overview diagrams
  show nothing.
- **Subsystems by layer** (links, services, application). That is what the layers already say; subsystems would then
  add nothing, and a link's driver and its protocol would sit in different bundles although they change together.

## Requirements

- NF-REQ-003 (corrections keep working without the server: `corrections` does not depend on `server_link`)
- NF-REQ-030 (each subsystem's pure logic is its own host-tested core)
- F-REQ-051 (the autopilot's traffic is relayed by its own subsystem, which never reads payloads)
- F-REQ-080 (the phone link is its own subsystem, added without touching the others)
- NF-REQ-001, NF-REQ-002, NF-REQ-004 (recovery is owned by one subsystem, `system`)

## Rules

- RULE-030
- RULE-031
- RULE-032
- RULE-033

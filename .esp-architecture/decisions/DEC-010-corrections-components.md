# DEC-010 Corrections components

## Context

The `corrections` subsystem (DEC-001) runs the NTRIP client: a TCP session to the caster over the cellular data
connection, the autopilot's GGA sentences sent up, the caster's bytes passed to the autopilot. It must keep working
while the server is down. As built (`main/ntrip.c`, verified on the board 2026-11-20 without the autopilot) it is one
module plus the pure `ntrip_codec` unit, and it is started by `app_main` by name.

## Decision

| Component | Layer | Owns |
|---|---|---|
| `ntrip` | services | The caster session on its own socket, stored caster settings, `ntrip_set` / `ntrip_clear`, the GGA schedule, the `ntrip` state field, its byte counts |
| `ntrip_codec` | pure | The request, parsing the caster's answer, decoding chunked streams, checking GGA sentences |

- `ntrip` sends corrections and receives GGA through `board_link` directly; it never uses `mqtt_link`.
- It starts its session itself when `link_state` shows the cellular link up (DEC-003), not on a call from `app_main`.
- The caster password lives only in the settings store and in the request to the caster; logs use a masked copy.

## Alternatives

- **Corrections inside `autopilot_link`.** The caster session has its own settings, commands, retry policy and network
  connection; it shares only the frames it sends with the relay.
- **Corrections over MQTT through the server.** Positioning would stop whenever the server is down.

## Requirements

- F-REQ-040 (stored caster settings)
- F-REQ-041 (the caster session)
- F-REQ-042 (corrections passed on unchanged)
- NF-REQ-003 (independent of the server)
- NF-REQ-010 (the password is never logged)
- NF-REQ-012 (the caster settings are not in the image)
- NF-REQ-030 (`ntrip_codec` is pure and host-tested)

## Rules

- RULE-001
- RULE-023

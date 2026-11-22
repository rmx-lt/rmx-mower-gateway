# DEC-008 Gateway services components

## Context

The `gateway_services` subsystem (DEC-001) holds what the gateway offers the server over MQTT: commands, firmware
update, its state, metrics and monitor streams. As built these modules form two cycles (`remote_cmd` and `ota`,
`metrics` and `monitor`), and links call up into them (`cell` and `mqtt_link` into `usage`, `metrics` and `gwstate`).
`usage` counts bytes with hooks on the network interface and per MQTT topic, on behalf of the links.

## Decision

| Component | Layer | Owns |
|---|---|---|
| `commands` | services | Command queue, registry and acks; the built-in `ping` and `restart`; `busy` while an update runs |
| `cmd_core` | pure | Command parsing, the remembered command ids, expiry checks |
| `firmware_update` | services | The `ota` command: chunk download, image checks, rollback, the result after the restart |
| `fw_chunk` | pure | The chunk header and its checks |
| `gateway_state` | services | The `gateway/state` message; other services set their own fields in it |
| `metrics` | services | Catalogue, telemetry, the signal and system metrics, the data-usage metrics from each link's counts |
| `monitor` | services | Monitor streams and their budget |

- `firmware_update` sets "update running" in `link_state`; `commands` reads it there, so `commands` does not depend on
  `firmware_update`. Only `firmware_update` depends on `commands` (it registers `ota`).
- Only `monitor` depends on `metrics`.
- `usage` is folded into `metrics`: each link counts its own bytes (`cell` on its network interface, `mqtt_link` per
  topic, the NTRIP session on its socket), and what is left is defining the usage metrics, which is the catalogue's
  job.
- Services of other subsystems register into these ones sideways: corrections registers its commands and usage
  metrics, the autopilot relay registers `autopilot_reset` and sets `autopilot_link` in the state.

## Alternatives

- **Keep `usage` as its own component.** It would only define metrics that belong in the catalogue, after the counting
  moved into the links.
- **`commands` asks `firmware_update` whether an update runs.** Keeps the cycle, since the update service also
  registers its command with `commands`.
- **One `gateway_services` component.** Commands, update, state and metrics change for different reasons and have
  different users; one component would be the box everything touches.

## Requirements

- F-REQ-012 (`gateway_state`)
- F-REQ-020, F-REQ-021, F-REQ-022 (`commands`, `cmd_core`)
- F-REQ-030, F-REQ-031, F-REQ-032 (`firmware_update`, `fw_chunk`)
- F-REQ-060, F-REQ-061, F-REQ-062 (`metrics`)
- F-REQ-063 (`monitor`)
- NF-REQ-010 (commands with secret arguments are logged by name and id only)
- NF-REQ-021 (monitor streams stay within their budget, one download at a time)
- NF-REQ-030 (`cmd_core`, `fw_chunk` are pure and host-tested)

## Rules

- RULE-001
- RULE-002
- RULE-023
- RULE-031
- RULE-032

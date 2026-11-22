# DEC-002 Five layers, pure core at the bottom

## Context

Layers say which way dependencies may point: a component uses only its own layer or the ones below. The as-built
scheme (reviewed 2026-11-19) has L4 app, L3 services, L2 pure-C codecs, L1 links, L0 platform. Two things in it make
dependencies point the wrong way or sideways:

- The pure-C codecs sit in the middle, so a link (L1) cannot use them, although they depend on nothing at all.
- The MQTT client is a service (L3), so every other service reaches it sideways; the sideways web is where the
  as-built cycles grew (`remote_cmd` and `ota`, `metrics` and `cell`).

## Decision

Five layers, from the top:

| Layer | Holds |
|---|---|
| `app` | Start-up order and the supervisor |
| `services` | Commands, firmware update, gateway state, metrics, telemetry, monitor, the relay protocol, the NTRIP session, the phone protocol |
| `links` | Transports to the outside: modem and PPP, the MQTT client with its identity, the board-link UART, Wi-Fi |
| `platform` | What every subsystem uses on the board: settings store, task table, link state, clock |
| `pure` | Protocol and decision logic with no ESP-IDF or FreeRTOS includes, tested on a PC; time is passed in |

- The MQTT client is a link: it is the server's transport, used by services from below like PPP.
- `pure` is the bottom layer: it depends on nothing, so every layer may use it, and the checker keeps it free of
  ESP-IDF.
- Sideways dependencies inside a layer stay allowed (a service registers its commands with the command service), but
  never in a cycle.

## Alternatives

- **Keep the as-built L0–L4.** Pure code stays out of reach of links, and the MQTT client keeps its users sideways.
- **MQTT client as a service.** Less code movement, but every service keeps a sideways dependency on it for its main
  path.
- **No sideways dependencies at all.** Would force the command registry below the services that register with it,
  into a layer it does not belong to.

## Requirements

- NF-REQ-030 (the `pure` layer is what runs in host tests)
- F-REQ-001 (the MQTT client is the server's only transport, so it is a link)
- F-REQ-010 (the protocol's gateway side is built on that transport by the services above it)

## Rules

- RULE-001
- RULE-002
- RULE-024

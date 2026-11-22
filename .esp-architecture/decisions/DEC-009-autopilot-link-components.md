# DEC-009 Autopilot link components

## Context

The `autopilot_link` subsystem (DEC-001) connects the gateway to the autopilot board over UART2 (Mower-Protocol §14):
COBS frames with a CRC, reliable frames with acks and resends, a heartbeat every second, MQTT messages relayed under the
autopilot's topics, and a reset line. It is not built yet. Two users send frames on it: the relay of the autopilot's
MQTT traffic and the NTRIP corrections. The pure parts (COBS, CRC-16, frame packing, reliable-frame state, the offline
queue) already exist and are host-tested.

## Decision

| Component | Layer | Owns |
|---|---|---|
| `board_link` | links | UART2 with RTS/CTS, the reset line, framing, reliable delivery, heartbeat, link up/down/incompatible; the only writer to UART2 |
| `bl_codec` | pure | COBS, CRC-16, frame packing, reliable-frame state, the offline queue |
| `relay` | services | The autopilot's topics to and from MQTT, prefix added and removed, the subscription list, messages held while MQTT is down, the `autopilot_reset` command, the `autopilot_link` state field |

- The transport (`board_link`) sits below both of its users, `relay` and the NTRIP session, like `mqtt_link` sits below
  the services.
- No separate UART component: `board_link` is UART2's only user, and ESP-IDF's UART driver is the bus driver.
- `board_link`'s receive and send tasks have the highest priorities of the firmware: at 921600 baud the receiver must
  keep up, and RTS/CTS covers only a short delay.
- `relay` never reads payloads; it handles topics, flags and sequence numbers only.

## Alternatives

- **One `board_link` component with the relay inside.** The NTRIP session would then depend on the MQTT relay to send
  corrections, and corrections would no longer be independent of the server side.
- **A separate UART driver component.** One user, nothing to adapt beyond ESP-IDF's driver.
- **Relay frames through the default event loop.** Mower-Protocol traffic is up to a few kilobytes per second; the
  event loop is for state changes, not data.

## Requirements

- F-REQ-050 (framing)
- F-REQ-051 (relay without reading payloads)
- F-REQ-052 (reliable frames)
- F-REQ-053 (subscriptions)
- F-REQ-054 (messages held while MQTT is down)
- F-REQ-055 (heartbeat)
- F-REQ-056 (reset line)
- NF-REQ-030 (`bl_codec` is pure and host-tested)

## Rules

- RULE-010
- RULE-033
- RULE-042
- RULE-043

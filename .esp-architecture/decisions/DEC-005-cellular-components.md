# DEC-005 Cellular components

## Context

The `cellular` subsystem (DEC-001) owns the LTE data connection. As built, `board` holds every pin of the board (modem
control, the user LED) and `cell` drives the modem, PPP and redial. `cell` also calls up into services: it attaches its
network interface to `usage`, sets the `operator` field of the state message, and registers the signal metric.

## Decision

| Component | Layer | Owns |
|---|---|---|
| `modem_power` | links | The modem's control pins: PWRKEY pulse, FLIGHT, DTR, STATUS |
| `cell` | links | The modem connection and PPP, redial and power-cycle, AT sessions while online (DEC-006), the signal reading once a minute, the operator code, the network interface, the user LED |

- `cell` calls nothing above it. It exposes the last signal reading, the operator code and its network interface, and
  sets "cellular up" in `link_state`. The state, metrics and data-usage components read these from below.
- The user LED is part of `cell`: `cell` is its only user and they change together.
- `board` is split by user: the modem pins to `modem_power`, the LED pin to `cell`, the autopilot reset pin to
  `autopilot_link`.

## Alternatives

- **Keep `cell` calling `usage`, the state message and the metric registry.** Three upward dependencies from a link
  into services.
- **A separate LED driver component.** A few lines with one user; it would add a box and an interface for nothing.
- **Keep one `board` component for every pin.** Every pin user would depend on it although no two share a pin.

## Requirements

- F-REQ-001 (the data connection the server is reached over)
- F-REQ-004 (`modem_power`)
- F-REQ-005 (the LED follows PPP)
- NF-REQ-001 (redial, then power-cycle the modem)

## Rules

- RULE-001
- RULE-010
- RULE-012
- RULE-032

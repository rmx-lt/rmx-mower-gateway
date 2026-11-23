# DEC-011 Local link components

## Context

A phone near the mower should drive it over Wi-Fi with the same autopilot commands and live-driving setpoints the
server sends (F-REQ-080). The same phone sets the mower up: it carries the board's certificate request to the server and
the certificate chain back (F-REQ-072). The transport and its protocol are not defined yet. The ESP32 has a Wi-Fi radio that the
firmware does not use today.

## Decision

Two planned components:

| Component | Layer | Owns |
|---|---|---|
| `wifi_link` | links | The Wi-Fi connection to the phone |
| `phone_link` | services | Carrying the phone's autopilot commands and setpoints to `board_link`; handing the certificate request to the phone and the chain it brings back to `identity` |

- `phone_link` uses `board_link` like `relay` does, so the autopilot sees the phone's messages in the same frames as
  the server's.
- `phone_link` provisions through `identity`, the same way `provisioning_console` does (DEC-007), and restarts the board
  through `boot` when a chain is stored.
- `app_main` starts the local link before it waits for a certificate, so a blank board can be set up without LTE.
- Nothing else depends on them: adding or dropping the phone link does not change another subsystem.
- How the two decide who is in control when the phone and the server send at the same time is left to the phone
  protocol's own decision.

## Alternatives

- **Bluetooth instead of Wi-Fi.** The owner asked for Wi-Fi.
- **The phone talks to the server and the server to the mower.** Needs LTE and the server, which a phone next to the
  mower should not.

- **The phone holds or makes the key.** The key never leaves the board (F-REQ-070); a certificate request and a chain
  are not secret, so the phone can relay them.
- **Only the PC provisions.** The owner wants the user to set the mower up with the phone; the PC stays for the workshop.

## Requirements

- F-REQ-072 (the phone relays the certificate request and the chain)
- F-REQ-080

## Rules

- RULE-031

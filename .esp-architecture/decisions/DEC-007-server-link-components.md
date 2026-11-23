# DEC-007 Server link components

## Context

The `server_link` subsystem (DEC-001) is the gateway's MQTT connection to the server and the identity it logs in
with. As built, `mqtt_link` calls up into `usage` (per-topic byte counts) and `gwstate`, and `devcert` holds both the
key-and-certificate store and the serial provisioning console on UART0, with `creds` as a one-function wrapper for the
device name.

## Decision

All three components are in the `links` layer (DEC-002).

| Component | Owns |
|---|---|
| `mqtt_link` | The MQTT client over TLS: clean session, 15 s keep-alive, retained status and last will, the route table (gateway, autopilot and absolute topics), reassembly of fragmented messages, its own per-topic byte counts; reports "MQTT up" and "server online" to `link_state` |
| `identity` | The device key (made on the board, never sent anywhere), the certificate chain, the device name from the certificate |
| `provisioning_console` | The serial commands on UART0 that make a CSR, store a chain or clear it: the workshop way, with a PC |

- `mqtt_link` calls nothing above it: it keeps its own per-topic counts, read by data-usage counting from below.
- `devcert` and `creds` become `identity` and `provisioning_console`, so `mqtt_link` does not depend on a UART0 command
  parser.
- `identity` checks a chain against the board's key whoever brings it: the PC through `provisioning_console` or the phone
  through `phone_link` (DEC-011).
- The server-link rule (up, lost after 10 s) stays in `link_state` (DEC-003); `mqtt_link` only reports its two facts.
- TLS trusts only the server's root certificates, set in the build configuration `mqtt_link` uses.
- `mqtt_link` keeps the contract's topic helpers (ack, firmware request), so the gateway's topics are named in one
  place.

## Alternatives

- **Keep `mqtt_link` calling `usage`.** An upward dependency from a link into services.
- **Keep `devcert` whole.** The MQTT client would depend on the console and its UART.
- **Only a generic publish in `mqtt_link`.** Each service would build topic names itself, so they would be spread over
  many files.

## Requirements

- F-REQ-001 (MQTT over TLS is the only way to the server)
- F-REQ-010 (topics, clean session, keep-alive, status and last will)
- F-REQ-014 (fragmented messages are put back together)
- F-REQ-070 (`identity`)
- F-REQ-071 (`provisioning_console`)
- F-REQ-072 (`identity` makes the request and checks the chain)
- NF-REQ-010 (the key is never logged or sent)
- NF-REQ-011 (trust only the server's roots)
- NF-REQ-012 (the key and certificate live only in the board's flash, never in the image)

## Rules

- RULE-001
- RULE-020
- RULE-021
- RULE-032

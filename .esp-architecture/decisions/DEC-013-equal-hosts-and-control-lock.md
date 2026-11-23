# DEC-013 Equal hosts and the control lock

## Context

A phone on the local link can do what the server can (F-REQ-080), and a host can take the mower for itself, for example
while it drives (F-REQ-023). DEC-008, DEC-009 and DEC-011 connect each service to `mqtt_link` or `phone_link` directly,
so every service would have to know both hosts and check the lock itself.

## Decision

- Commands and messages from every host pass one point, and the services behind it do not know which host sent them.
  An answer goes back to the host that asked; the mower's state goes to every connected host.
- The lock is checked at that one point, before a command reaches a gateway service or the autopilot.
- The gateway knows two hosts, the server and the phone, by the connection a message came over. It does not need to know
  the devices behind the server, and it still does not read autopilot payloads (F-REQ-051): the topic tells whether a
  message is a command or a setpoint.
- The mode and its holder are part of the gateway's state, so a web interface can disable its controls.

Where this point lives among the components is decided with the block diagram. Until then the manifest keeps the
components of DEC-008, DEC-009 and DEC-011, and `commands` carries F-REQ-023.

## Alternatives

- **The autopilot holds the lock.** It could not lock the gateway's own commands, such as a restart or an update.
- **The server holds the lock alone.** It does not see a phone that is connected directly.
- **A session id in every command.** The gateway would have to read autopilot payloads to check it.
- **`stop` from any host in locked mode.** The owner wants one simple rule: only the holder is obeyed.

## Requirements

- F-REQ-023
- F-REQ-080

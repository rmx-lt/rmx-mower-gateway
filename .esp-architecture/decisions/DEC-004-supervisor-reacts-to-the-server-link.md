# DEC-004 The supervisor alone reacts to the server link coming up

## Context

When the server link comes up, several things must happen in order: a new firmware image confirms itself, then the
gateway publishes its state (whose `previous` field the confirmation sets) and its metric catalogue, then the result of
a pending update is reported. Done in another order, the first state after an update would carry `previous: null`;
that ordering was verified on the board on 2026-11-20.

## Decision

The supervisor is the only component that reacts to the server link coming up. On that event it, in order: confirms
the image, publishes the gateway state and the metric catalogue, and reports the update result. It also never runs a
recovery step while an image is being written.

## Alternatives

- **Each service reacts to the link event itself.** Fewer dependencies for the supervisor, but the order between
  services would depend on event delivery order, and the verified "no state with `previous: null`" would no longer be
  guaranteed.
- **The update service publishes the state after confirming.** Puts the state message's timing inside the update
  service, which then has to know about state and catalogue publishing.

## Requirements

- F-REQ-011 (state and catalogue are published again when the server link comes up)
- F-REQ-012 (state)
- F-REQ-032 (the image confirms when it reaches the server, and the result follows)
- NF-REQ-005 (no recovery step during an image write)

## Rules

- RULE-033

# DEC-003 System components

## Context

The `system` subsystem (DEC-001) runs the board: start-up, recovery, and the few services every other subsystem needs.
In the as-built code these jobs are spread over `app_main`, `supervisor`, `gwstate`, `settings`, `gw_tasks`, `gw_link`
and the codec's `recovery` unit, with two wrong-way dependencies:

- `gw_restart()` lives in `supervisor` (top layer) but is called from lower layers.
- `gwstate` mixes the boot counter, the clock, a small flash-storage helper and the `gateway/state` message, so every
  user of the boot counter also depends on the state message.

`app_main` also sets the clock itself and starts the NTRIP client directly, although it should only set the start-up
order.

## Decision

| Component | Layer | Owns |
|---|---|---|
| `app_main` | app | Start-up order only: starts each subsystem in turn |
| `supervisor` | app | The recovery ladder and the stuck-task check; reacts to the server link (DEC-004) |
| `boot` | platform | Boot counter, restart with a logged reason, the crash-dump notice at start |
| `clock` | platform | Network time, the "clock valid" state, time for messages |
| `settings` | platform | The only owner of flash storage (NVS) |
| `tasks` | platform | Task table, task creation, liveness counters |
| `link_state` | platform | Link-state bits and change events |
| `recovery` | pure | Recovery and liveness decisions, host-tested |

- `gwstate` is split: boot counter to `boot`, clock to `clock`, storage helpers to `settings`, the `gateway/state`
  message to `gateway_services`.
- The restart function moves to `boot`, so lower layers call down.
- Network-time setup moves from `app_main` to `clock`.
- `tasks` is used by every component that owns a task and by `supervisor`.
- No subsystem is started by name from another: `app_main` starts each subsystem, and a subsystem that waits for a
  link (corrections waits for the cellular link) watches `link_state`.

## Alternatives

- **Keep `gwstate` whole.** Every user of the boot counter or the clock would also pull in the state message and its
  JSON library.
- **Keep the restart function in `supervisor`.** Every lower-layer caller is then an upward dependency.
- **Let `app_main` start dependent subsystems in order of readiness** (cellular, then corrections). `app_main` would
  have to know every subsystem's readiness rules; with `link_state` each subsystem keeps its own.

## Requirements

- F-REQ-002 (`clock`)
- F-REQ-011 (`link_state` carries the server-link facts)
- F-REQ-013 (`boot`)
- NF-REQ-002, NF-REQ-004 (`supervisor`, `recovery`, `tasks`)
- NF-REQ-006 (`boot` keeps and reports the crash dump)
- NF-REQ-007 (`settings` is the only flash-storage owner)
- NF-REQ-030 (`recovery` is pure and host-tested)

## Rules

- RULE-001
- RULE-020
- RULE-022
- RULE-032
- RULE-033

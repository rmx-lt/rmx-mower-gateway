# DEC-006 Pause PPP to send AT commands while online

## Context

The modem has one UART. While PPP runs on it, AT commands (the signal reading once a minute, the operator code) need a
way to the modem's command interpreter. Two ways were measured on the board:

- **Pause PPP**: leave data mode (`+++`), send the commands, return (`ATO`). Verified 2026-10-20: the PPP session and
  open TCP connections survive; data stops for 2–3 s.
- **CMUX**: one virtual channel for PPP and one for AT at the same time. Verified on the bench 2026-11-19: it works, but
  re-entering CMUX after leaving it gave no IP within 60 s, and it changes the whole start-up and redial path.

## Decision

Pause PPP. `cell` serialises every AT session with the redial task, so only one of them talks to the modem at a time.
AT is needed about once a minute, so a 2–3 s pause is cheap. CMUX is revisited when AT is needed often or the pause
shows up in the RTK corrections (tracked as rmx-mower-gateway#98).

## Alternatives

- **CMUX.** No pause in data, but the failed re-entry is unexplained and the redial path would be rewritten.
- **A second UART to the modem.** The board does not wire one.

## Requirements

- F-REQ-003 (read the modem while online without ending the PPP session)

## Rules

- RULE-033

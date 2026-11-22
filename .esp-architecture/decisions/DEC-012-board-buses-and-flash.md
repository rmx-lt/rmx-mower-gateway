# DEC-012 Board, buses and flash layout

## Context

The firmware runs on one board, the LilyGo T-SIM7600 (ESP32-WROVER-E with 16 MB flash and 8 MB PSRAM, SIM7600G-H
modem). Its pins are fixed by the board and by the wiring to the autopilot. The flash holds the firmware images, the
settings store and the crash dump, and every update must leave room for the running image.

## Decision

Buses and the components that own them:

| Bus | Peripheral, pins | Device | Owner |
|---|---|---|---|
| Modem UART | UART1, TX 27, RX 26, no flow control | SIM7600 | `cell` |
| Modem control | GPIO 4 PWRKEY, 25 FLIGHT, 32 DTR, 34 STATUS | SIM7600 | `modem_power` |
| User LED | GPIO 12 (strapping pin, low at reset) | blue LED | `cell` |
| Board link | UART2, TX 23, RX 19, RTS/CTS | autopilot board | `board_link` |
| Autopilot reset | GPIO 21, open drain | autopilot board | `board_link` |
| Console | UART0 through the CH9102 USB bridge | provisioning PC | `provisioning_console` |
| Wi-Fi | the ESP32's radio | phone | `wifi_link` |
| Flash | SPI0/1, inside the module | flash | ESP-IDF's partition and NVS drivers |

- No bus is shared between components, except the flash.
- The flash is shared through ESP-IDF's partition and NVS drivers: `settings` owns the NVS partition, `firmware_update`
  writes only the spare app slot, `boot` reads only the core dump partition.
- Flash layout: two 4 MB app slots (`ota_0`, `ota_1`), an NVS partition for settings and identity, a 128 KB core dump
  partition, the rest spare.
- Everything that goes to the server shares the modem's UART, about 11 KB/s; monitor budgets and one download at a time
  keep within it.

## Alternatives

- **One 1 MB app slot or no update slot.** The image is about 0.8 MB and grows; an update needs a second slot.
- **An encrypted NVS partition.** Not required now; deferred by the owner (rmx-mower-gateway#49).

## Requirements

- NF-REQ-006 (the core dump partition)
- NF-REQ-007 (NVS survives flashing and updates)
- NF-REQ-020 (the T-SIM7600 board and its pins)
- NF-REQ-021 (the modem's bandwidth is shared by all server traffic)
- NF-REQ-022 (two app slots)

## Rules

- RULE-010
- RULE-011
- RULE-013

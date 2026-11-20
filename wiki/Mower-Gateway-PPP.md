# Mower-Gateway-PPP: esp_modem + PPP on this board (the chosen data path)

Component: `espressif/esp_modem` **2.1.0** (registry) · source + examples:
`C:\Repos\esp-protocols\components\esp_modem` · docs:
<https://docs.espressif.com/projects/esp-protocols/esp_modem/docs/latest/index.html>.
Working reference code: [`tools/ppp_smoke/main/ppp_smoke.c`](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/tools/ppp_smoke/main/ppp_smoke.c).

## Why PPP

The modem becomes a plain IP pipe. lwIP, DNS, TLS (mbedTLS) and MQTT (esp-mqtt) run on the
ESP32 with standard ESP-IDF APIs. That gives TLS 1.3/1.2, the ISRG cert bundle, normal error
codes, and code that also works over Wi-Fi. The gateway firmware speaks MQTT only.
`tools/ppp_smoke` still does one HTTPS GET to check the pipe.
The modem's own TLS (02, option B) works too but is a fallback.

## Verified result (2026-10-19, 3 runs)

The log is from that day: the HTTPS check then went to `api.rmx.lt/probe/health`, a host and
path that no longer exist. `tools/ppp_smoke` now gets `https://app.rmx.lt/health`.

```
module SIMCOM_SIM7600G-H, rssi 19..31
PPP up: ip 10.57.66.246 gw 10.64.64.64 dns 83.181.60.98      (connect ~160 ms after ATD)
HTTPS GET https://api.rmx.lt/probe/health -> 200 in ~4.4 s: {"status":"ok","db":true,"mqtt":true,"version":"0.1.0"}
mqtts://mqtt.rmx.lt:8883, invalid login -> "Connection refused, not authorized" (CONNACK 5) in ~4.3 s
free heap 4.44 MB (incl. PSRAM)
```

Each TLS handshake takes ~4.3 s (LTE round trips + ECDSA P-384 chain verification in
software). Keep connections open (MQTT keepalive, HTTP keep-alive) instead of reconnecting.

Also verified: resetting the ESP32 while the modem was in PPP data mode → the next boot
still connected normally (the `DETECT` branch didn't log a data-mode switch, so the exact
recovery path is not characterized; keep the DETECT + sync + PWRKEY fallback sequence).

## The board-specific config (most important part)

`ESP_MODEM_DTE_DEFAULT_CONFIG()` defaults are **wrong for this board**:
`tx 25, rx 26, rts 27, cts 23`. GPIO25 is FLIGHT and GPIO27 is our TX. Override all four:

```c
esp_modem_dte_config_t dte = ESP_MODEM_DTE_DEFAULT_CONFIG();
dte.uart_config.tx_io_num  = 27;
dte.uart_config.rx_io_num  = 26;
dte.uart_config.rts_io_num = -1;              // not wired
dte.uart_config.cts_io_num = -1;              // not wired
dte.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
dte.uart_config.rx_buffer_size = 8192;        // default 4096
dte.uart_config.tx_buffer_size = 2048;
dte.dte_buffer_size = 4096;
esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG("internet");   // APN
esp_netif_config_t ppp_cfg = ESP_NETIF_DEFAULT_PPP();
esp_netif_t *ppp = esp_netif_new(&ppp_cfg);
esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte, &dce_cfg, ppp);
```

The same wrong defaults are in the `pppos_client` example's Kconfig (TX 25, RTS 27).

Before esp_modem: GPIO25 (FLIGHT) HIGH, GPIO32 (DTR) LOW, GPIO4 (PWRKEY) LOW.

## Startup sequence (works)

1. `esp_netif_init()`, default event loop, register `IP_EVENT` handler
   (`IP_EVENT_PPP_GOT_IP` / `IP_EVENT_PPP_LOST_IP`).
2. Board pins (above), create DTE/DCE.
3. `esp_modem_set_mode(dce, ESP_MODEM_MODE_DETECT)`; if the mode is DATA → `set_mode(COMMAND)`.
4. `esp_modem_sync()`; if it fails → PWRKEY HIGH 500 ms, retry sync for ~30 s.
5. Wait for `esp_modem_get_signal_quality()` rssi ≠ 99 (registration).
6. `esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA)` → sets PDP context (`AT+CGDCONT=1,"IP","internet"`)
   and dials `ATD*99#`; wait for `IP_EVENT_PPP_GOT_IP` (≤ 60 s).
7. Use sockets/TLS/MQTT/HTTP normally.
8. Before a planned reset: `esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND)`.

## SIM7600 in esp_modem

`ESP_MODEM_DCE_SIM7600` (`command/src/esp_modem_modules.cpp`) adds over the generic DCE:
`get_battery_status` (`AT+CBC`, SIM7xxx "3.838V" format), `set_network_bands` (`AT+CNBP`),
`set_gnss_power_mode`, `power_down` (`AT+CPOF`). Data mode uses `ATD*99#` (generic).
Useful C API calls: `esp_modem_sync`, `esp_modem_get_signal_quality`, `esp_modem_get_imsi`,
`esp_modem_get_imei`, `esp_modem_get_module_name`, `esp_modem_get_operator_name`,
`esp_modem_read_pin`/`set_pin`, `esp_modem_at(dce, "AT+CPSI?", out, timeout)` (raw AT),
`esp_modem_pause_net(dce, true/false)` (temporarily leave PPP to send AT), `esp_modem_destroy`.

## AT commands while online: `esp_modem_pause_net()` (chosen, verified 2026-10-20)

The probe firmware uses `esp_modem_pause_net(dce, true)` → AT commands → `esp_modem_pause_net(dce, false)`,
wrapped in `cell_with_command_mode()` (`main/cell.c`) with a mutex shared with the redial watchdog.
What it does inside esp_modem 2.1.0: stop feeding lwIP frames to the UART, wait 1 s (escape guard
time), send `+++` (expects `OK`/`NO CARRIER`), then later `ATO` (expects `CONNECT`) and resume.

Measured: `+++` ~1.9–2.0 s (incl. the 1 s guard), GNSS AT commands 30–50 ms, `ATO` ~10 ms.
PPP resumed every time (4 sessions); the MQTT link and the HTTPS keep-alive connection survived:
the probe-server request right after the pause answered in ~400 ms. No reconnect in the log.
If `ATO` failed, the code puts the DCE into command mode and lets the watchdog redial (untested).
`esp_modem_at()` returns the last response line before `OK` (truncated to
`CONFIG_ESP_MODEM_C_API_STR_MAX` = 128 chars).

Why not CMUX (re-checked on the board 2026-11-19, see the next section): CMUX works, but it
changes the whole DTE/redial path (mode transitions, a failed re-entry), and today the firmware
needs AT only once a minute (signal reading), so a 2 s pause is cheap. Revisit when AT is needed
often or the 2 s gap starts to hurt a stream (NTRIP corrections, board-link relay).

## AT commands while online: CMUX (bench, verified 2026-11-19)

`AT+CMUX=?` → `(0),(0),(1-8),(1-1500),…` — basic-mode CMUX only.
`esp_modem_set_mode(dce, ESP_MODEM_MODE_CMUX)` gives one virtual channel for PPP and one
for AT at the same time. Example: `examples/simple_cmux_client`. Without CMUX, the only
way to send AT while online is `esp_modem_pause_net()` (one UART, no second AT port).

Bench: `tools/ppp_smoke` changed to enter CMUX, then run AT (`AT+CSQ` + `AT+CPSI?`, every 200 ms)
on the command channel while PPP carried 5 HTTPS GETs and one MQTTS connect (esp_modem 2.1.0,
ESP-IDF 6.1, modem fw B04, 115200 baud, no flow control). The bench code is not in the repo.

| Check | Result (runs) |
|---|---|
| Enter CMUX from command mode, PPP up | 244–344 ms to IP (3 of 3) |
| AT on the command channel during PPP traffic | 84–85 ok, 0 failed per run, 130–280 ms each, max 490–620 ms (3 of 3) |
| PPP during AT | HTTPS `/health` 200 in 5.1–5.5 s each, MQTTS CONNACK 5 in ~5.1 s: same as without CMUX |
| Exit CMUX to command mode (`ESP_MODEM_MODE_COMMAND`) | `ESP_OK` in ~70 ms, `AT` answers (3 of 3) |
| **Re-enter CMUX after an exit**, same modem session | entered `ESP_OK`, but **no IP within 60 s** (1 of 1) |
| ESP reset while the modem is in CMUX: `ESP_MODEM_MODE_DETECT` | reports mode 3 (`CMUX_MANUAL`); `CMUX_MANUAL_EXIT` → command mode, `AT` OK (2 of 3); first run: exit `ESP_FAIL` and entering CMUX failed (earlier bench code, cause not isolated) |
| Heap | ~4.45 MB free (PSRAM), no change worth noting |

What this means:

- CMUX itself is stable on this board and removes the 2 s PPP gap: no truncated answers seen at a
  200 ms AT rate under light PPP load (heavy load, e.g. a 700 KB OTA download, was not tested).
- The weak spot is mode changes. A redial that leaves CMUX and enters it again did not get an IP.
  A CMUX firmware must treat "leave CMUX" as a modem power cycle (or test `AT+CRESET`), and its
  boot must handle a modem left in CMUX by the previous ESP boot (detect works, not every time).
- The firmware's whole AT path (`cell_with_command_mode`, redial, power cycle) would change.

**Recommendation: keep `esp_modem_pause_net()`** while AT is needed once a minute. Switch to CMUX
only when a feature needs AT often while data flows; then implement the recovery rules above first
(follow-up issue [rmx-mower-gateway#98](https://github.com/rmx-lt/rmx-mower-gateway/issues/98)).

## Robustness notes (for the real firmware)

- Handle `IP_EVENT_PPP_LOST_IP`: go to command mode, check registration, redial. Cap retries
  and power-cycle the modem (PWRKEY off/on) as the last resort.
- The ESP and modem reset independently. Never assume the modem state at boot.
- No hardware flow control: keep the baud at 115200 (fine for MQTT telemetry). Higher
  rates without RTS/CTS risk UART overruns; enable `CONFIG_UART_ISR_IN_IRAM` if raising it.
- `CONFIG_ESP_MODEM_ADD_DEBUG_LOGS=y` logs all UART traffic (debugging only).
- The cellular IP is CGNAT: inbound connections are impossible; MQTT (device-initiated) is the right model.

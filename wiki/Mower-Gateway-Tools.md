# Mower-Gateway-Tools: tools/

Diagnostic firmware and scripts for the LilyGo T-SIM7600. All were run on the real board.

| Tool | What it does |
|---|---|
| [`at_bridge/`](https://github.com/rmx-lt/rmx-mower-gateway/tree/development/tools/at_bridge) | ESP-IDF app: powers the modem with the board's pin sequence, then bridges USB console ⇄ modem UART (like LilyGo's ATdebug). |
| [`at_session.py`](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/tools/at_session.py) | Host script: resets the ESP, waits for `at_bridge`, sends AT commands, prints replies. |
| [`serial_log.py`](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/tools/serial_log.py) | Host script: resets the ESP and prints its log (`COM6 [seconds] [--until TEXT]`). |
| [`mqtt_cmd.ps1`](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/tools/mqtt_cmd.ps1) | Host script: sends a remote command to the probe firmware (`mower/<device>/cmd`, `-Device`, default `mower-01`; login `app`) and prints the reply from `…/cmd/ack`. |
| [`provision_nvs.ps1`](https://github.com/rmx-lt/rmx-mower-gateway/blob/development/tools/provision_nvs.ps1) | Host script: writes the device name (`-Device`, default `mower-01`) and the MQTT password into the board's NVS (hidden prompt or `RMX_MQTT_PW`), without printing the password. Once per board, or to switch the board to another identity (no rebuild). Replaces the whole NVS partition. Verified on hardware 2026-11-08: self-test passes with NVS credentials; with empty NVS it fails with a clear `creds` error, no crash. |
| [`ppp_smoke/`](https://github.com/rmx-lt/rmx-mower-gateway/tree/development/tools/ppp_smoke) | ESP-IDF app: esp_modem PPP → HTTPS GET `api.rmx.lt/probe/health` → MQTTS to `mqtt.rmx.lt:8883` with a fake login (expects CONNACK 5). The reference for the probe firmware. |

## Build and flash (PowerShell)

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd tools\at_bridge          # or tools\ppp_smoke
idf.py set-target esp32     # first time only
idf.py build
idf.py -p COM6 -b 921600 flash
```

Only one app is on the board at a time; flashing one replaces the other.

## AT sessions (with at_bridge flashed)

```powershell
$py = "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
& $py -I -u tools\at_session.py COM6 "AT+CSQ" "AT+CPSI?" "AT+COPS?"
& $py -I -u tools\at_session.py COM6 --file cmds.txt
& $py -I -u tools\at_session.py COM6 --listen 30
```

Command prefixes: `wait=<ms>:` (timeout), `expect=<text>|` (wait for a URC such as
`+CCHOPEN:` instead of `OK`), `raw:` (send bytes without CR; `\r\n` escapes),
`rawfile:<path>` (send a file, e.g. a PEM after `AT+CCERTDOWN`). Example file for a
verified TLS test: see [Mower-Gateway-SIM7600](Mower-Gateway-SIM7600).

Bridge commands (type a line starting with `~`): `~status` (STATUS/RING/FLIGHT/DTR levels),
`~pwr` (500 ms PWRKEY pulse — toggles power!), `~pwroff` (3 s), `~flight0`/`~flight1`, `~baud N`.

## Reading the ESP log without `idf.py monitor`

Opening COM6 with pyserial toggles DTR/RTS and **resets the ESP32** (auto-reset circuit).
The modem is not reset. Minimal reader:

```python
import serial, time
s = serial.Serial("COM6", 115200, timeout=0.2)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False   # clean reset
t = time.time()
while time.time() - t < 60:
    print(s.read(4096).decode("utf-8", "replace"), end="")
```

## PlatformIO build (alternative to idf.py)

`platformio.ini` builds the probe firmware (`main/`) with PlatformIO next to `idf.py`; `idf.py` stays the reference build and
`tools/ppp_smoke` and `tools/at_bridge` are idf.py only. Both builds read `sdkconfig.defaults` and `partitions.csv`.

```powershell
~\.platformio\penv\Scripts\pio.exe run -e probe              # build (first build ~13 min, downloads ~1.5-2 GB)
~\.platformio\penv\Scripts\pio.exe run -e probe -t upload    # flash on COM6
```

Verified on hardware 2026-11-08:
- Platform `espressif32 @ 7.1.0` (pinned) bundles ESP-IDF v6.1.0; the `pioarduino` fork is still on IDF 5.5. Managed components (esp_modem, mqtt, cjson) resolve from `main/idf_component.yml`; no `lib_deps`.
- PlatformIO writes `sdkconfig.probe` (gitignored, like `.pio/`).
- PlatformIO does **not** sign the image. `sign_ota.py` (post-build hook) signs `firmware.bin` with `keys/ota_signing_key.pem`; check with `espsecure.py verify_signature --version 2 --keyfile keys/ota_signing_key.pem .pio/build/probe/firmware.bin`.
- A changed `version.txt` is only picked up after a CMake reconfigure: delete `.pio/build/probe/CMakeCache.txt`, then build.
- Flashing with `pio ... -t upload` keeps NVS (credentials); the CH9102 auto-reset works with the default settings (no `monitor_rts`/`monitor_dtr` override needed); `serial_log.py` also works as before.
- The PlatformIO-built image passed the PPP/MQTT self-test. An HTTPS OTA to a PlatformIO-built, signed 0.2.4 image succeeded before firmware download moved to MQTT (`ota_0` to `ota_1`, confirmed).
- 2026-11-10: `pio run -e probe` signed `firmware.bin` (729088 bytes; RAM 20752/327680, flash 706493/4194304). USB flash of 0.3.0 on COM6 passed the self-test. `0.3.1.bin` (same tree, version string only) was copied to the server firmware dir and installed by `tools\mqtt_cmd.ps1 ota -Version 0.3.1 -Size 729088`. See [Mower-Gateway-Plan](Mower-Gateway-Plan).
- `tools/release.ps1` is idf.py only; for a PlatformIO image upload `firmware.bin` as `<version>.bin` by hand and send `tools\mqtt_cmd.ps1 ota`. Use PowerShell's `ssh`/`scp` (Git Bash's has no key).

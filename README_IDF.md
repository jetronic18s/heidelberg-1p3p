# Development (ESP-IDF 5.5.6)

Developer documentation: building, versioning, releases and tests. For the
project overview, installation and usage see [README.md](README.md) (German).

This repository uses an ESP-IDF-only build flow.

## Layout

- Repository root contains the ESP-IDF project (CMake, `sdkconfig.defaults`, `main/` component).
- Existing sources remain in `src/` and `include/`.
- Local project components live under `main/` and `lib/`.

## Dependencies

No repository-managed component fetch step is required. ESP-IDF resolves managed
components from `main/idf_component.yml` during configure/build.

- `espressif/esp-modbus` is managed by the ESP-IDF Component Manager.
- `lib/eth_phy_jl1101` is included directly as a local ESP-IDF component.

## Build

From the repository root:

```
. ~/esp-idf/export.sh
idf.py set-target esp32
idf.py build
```

Clean rebuild (after IDF or component changes):

```
rm -rf build managed_components dependencies.lock
. ~/esp-idf/export.sh
idf.py build
```

## Flash

From the repository root:

```
./scripts/flash.sh /dev/ttyUSB0
```

`flash.sh` writes fixed addresses from these binary paths:
- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`
- `build/ota_data_initial.bin`
- `build/heidelberg-1p3p.bin`

If those binaries are missing, it falls back to `idf.py flash`.

Or manually:

```
. ~/esp-idf/export.sh
idf.py -p /dev/ttyUSB0 flash monitor
```

To erase everything first, including NVS (WiFi credentials and config):

```
idf.py -p /dev/ttyUSB0 erase-flash
```

After booting, check `App version` in the log or `Version` at the bottom of the
status page.

## Versioning

The firmware version is derived from the latest `v*` Git tag
(`git describe --tags --match "v*" --dirty`):

- `v1.1.2`: exactly the tagged commit
- `v1.1.2-2-gfadc5d4b`: 2 commits after `v1.1.2`, with the commit hash
- `-dirty` suffix: built with uncommitted changes

New version: `git tag -a v1.1.3 -m "..."`. The version is determined at CMake
configure time, so run `idf.py reconfigure` (or `rm -rf build`) after a new
commit or tag before building.

## Release

1. Test the firmware, then tag and push: `git tag -a v1.1.3 -m "..."` and
   `git push origin v1.1.3`.
2. CI builds the tag and creates a draft release with
   `heidelberg-1p3p-<tag>.bin` (OTA), the flash bundle as ZIP, and release
   notes generated from the commits since the previous tag
   (`scripts/release_notes.sh`, grouped by `feat`, `fix` and the rest).
3. Review the draft on GitHub under *Releases*, polish the notes if needed and
   publish it.

## Checklist

Quick run per firmware build:

1. Build smoke test: `./scripts/smoke_test.sh`
2. Optional flash and short monitor:
   `./scripts/smoke_test.sh --port /dev/ttyUSB0 --monitor-seconds 20`
3. Optional Modbus read checks against a running device:
   `./scripts/smoke_test.sh --host <DEVICE_IP>`

## Debug Logging

On Dingtian builds, Modbus RTU uses UART0. When Modbus traffic is active, the USB serial monitor can show binary characters.

Enable **Telnet-Debug aktiv (Port 23)** in the web UI (`/config`) while Modbus is enabled, then connect:

```bash
telnet <DEVICE_IP> 23
```

or:

```bash
nc <DEVICE_IP> 23
```

## Smoke Test

From repository root:

```
./scripts/smoke_test.sh
```

Optional examples:

```
./scripts/smoke_test.sh --port /dev/ttyUSB0 --monitor-seconds 20
./scripts/smoke_test.sh --host 192.168.1.42
./scripts/smoke_test.sh --port /dev/ttyUSB0 --host 192.168.1.42
```

Notes:
- Modbus checks require `mbpoll` in `PATH`.
- Use `--skip-build` for quicker reruns.

## Modbus Regression And Stress Tests

From repository root:

```bash
./scripts/modbus_regression.sh --host 192.168.178.51
```

If no wallbox/RTU follower is connected yet, run bridge-only reachability:

```bash
./scripts/modbus_regression.sh --host 192.168.178.51 --bridge-only
```

Optional write/readback check (register 261, same-value roundtrip):

```bash
./scripts/modbus_regression.sh --host 192.168.178.51 --write-test
```

Parallel stress test with mixed normal/invalid/partial-frame traffic:

```bash
./scripts/modbus_stress.py --host 192.168.178.51 --clients 8 --duration-s 120
```

Notes:
- `modbus_regression.sh` requires `mbpoll`.
- `modbus_stress.py` uses Python stdlib only (no extra pip dependency).
- For first runs, keep `--max-transport-errors` at `0`; increase only if your network is noisy.
- Full wallbox checklist: `docs/MODBUS_TEST_PROTOCOL.md`.

## Ethernet/WiFi Handover Test

- Boot with LAN plugged in: WiFi must stay off.
- Unplug LAN: WiFi STA (or the setup AP without credentials) must start.
- Plug LAN back in: WiFi must stop cleanly (`wifi:mode : null`).
- Repeat several times, including short LAN flapping.

## Notes

- Ethernet setup currently relies on `ethernet_jl1101.*` and IDF ethernet APIs.
- Modbus TCP bridge uses an IDF socket server (`ModbusTcpBridge`), not AsyncTCP.
- RS485 RTU client uses an IDF UART-backed `Stream` adapter.

## Safety Fault Codes

See [Fehlercodes](README.md#fehlercodes) in the README.

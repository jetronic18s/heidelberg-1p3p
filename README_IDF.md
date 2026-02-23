# ESP-IDF (v5.5.2) Build

This repository uses an ESP-IDF-only build flow.

## Layout

- Repository root contains the ESP-IDF project (CMake, `sdkconfig.defaults`, `main/` component).
- Existing sources remain in `src/` and `include/`.
- External Arduino/libs should be placed under `components/`.

## Required Components

Fetch the required components into `components/`:

```
./scripts/fetch_components.sh
```

`components.lock` pins both the commit and the tree hash to ensure reproducible checkouts.
Use `./scripts/fetch_components.sh --no-verify` to skip tree hash verification.

The following libraries are expected as ESP-IDF components in `components/`:

- `arduino` (arduino-esp32 core, as an ESP-IDF component; pinned to 3.3.5)
- `eModbus` (pinned to ed343224...)
- `Uptime` (Uptime Library)

Each component must include its own `CMakeLists.txt` (or be adapted to one).

## Build

From the repository root:

```
. ~/esp-idf/export.sh
./scripts/fetch_components.sh
idf.py set-target esp32
idf.py build
```

## Flash

From the repository root:

```
./scripts/flash.sh /dev/ttyUSB0
```

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

`flash.sh` writes fixed addresses from these binary paths:
- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`
- `build/ota_data_initial.bin`
- `build/heidelberg-1p3p.bin`

If those binaries are missing, it falls back to `idf.py flash`.

Or manually:

```
. ~/esp-idf/export.sh
idf.py -p /dev/ttyUSB0 flash
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
- Use `--skip-fetch`/`--skip-build` for quicker reruns.

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

## Notes

- Ethernet setup currently relies on `ethernet_jl1101.*` and IDF ethernet APIs.
- Modbus TCP bridge uses an IDF socket server (`ModbusTcpBridge`), not AsyncTCP.
- RS485 RTU client uses an IDF UART-backed `Stream` adapter.

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
4. Before a release: flash the bench board via OTA and run the bench tests,
   `bench/bench_test.py` (see [Test Bench](#test-bench)).

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

## Test Bench

`bench/` contains a bench setup to test the firmware and new evcc versions
without a real wallbox: a bench board, a Heidelberg simulator on a
USB-RS485 adapter (`scripts/heidelberg_sim.py`), a local evcc instance, and
automated tests (`bench/bench_test.py`). Wiring, setup, simulator commands,
the test list and manual checks are described in [bench/README.md](bench/README.md).

```bash
ESP_IP=<DEVICE_IP> bench/start.sh --init   # once: bench directory and evcc config
bench/start.sh --background                # simulator + evcc
bench/bench_test.py                        # all tests
```

## Ethernet/WiFi Handover Test

- Boot with LAN plugged in: WiFi must stay off.
- Unplug LAN: WiFi STA (or the setup AP without credentials) must start.
- Plug LAN back in: WiFi must stop cleanly (`wifi:mode : null`).
- Repeat several times, including short LAN flapping.

## Notes

- Ethernet setup currently relies on `ethernet_jl1101.*` and IDF ethernet APIs.
- Modbus TCP bridge uses an IDF socket server (`ModbusTcpBridge`), not AsyncTCP.
- RS485 RTU client uses an IDF UART-backed `Stream` adapter.
- Outside `Running` (phase switch, restart) the bridge answers evcc from the
  register cache: status A1 or B1 depending on whether a car was connected
  before, currents, power and voltages 0. Writes go to the cache and are
  applied when the switch completes.
- evcc watchdog: the ESP32 polls the wallbox every 2 s and so keeps the
  Heidelberg watchdog (register 257) alive. It takes over that watchdog for
  the Modbus TCP side: without a request for the register 257 time it writes
  the failsafe current (register 262) to register 261 and shows
  `[evcc timeout]` on the start page. evcc's Heidelberg driver reads register
  257 at startup and polls every 257/4 ms, independent of its control
  interval.
- Wallbox silent in `Running` for 60 s (any failed RTU transaction counts,
  any success resets): the start page shows `[wallbox not responding]`, the
  status page a `Wallbox` row, and the bridge answers evcc with exception
  0x0B without trying the RTU. No safety fault: with RS485 cut, the
  Heidelberg's own watchdog has already stopped charging; everything resumes
  when it answers again.
- Before a software restart the cache is saved in RTC memory (magic + CRC)
  and restored after the restart, so evcc keeps getting cached values until
  the wallbox answers. After a power-on there is no cache; until the wallbox
  is detected the bridge answers Modbus exception 0x0B (gateway target
  failed).

## Safety Fault Codes

See [Fehlercodes](README.md#fehlercodes) in the README.

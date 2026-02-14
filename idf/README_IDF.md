# ESP-IDF (v5.5.2) Build

This project can be built with ESP-IDF directly while keeping the existing PlatformIO setup untouched.

## Layout

- `idf/` contains the ESP-IDF project (CMake, sdkconfig.defaults, main component).
- Existing sources remain in `src/` and `include/`.
- External Arduino/libs should be placed under `idf/components/`.

## Required Components

Fetch the required components into `idf/components/`:

```
./scripts/fetch_components.sh
```

`components.lock` pins both the commit and the tree hash to ensure reproducible checkouts.
Use `./scripts/fetch_components.sh --no-verify` to skip tree hash verification.

The following libraries are expected as ESP-IDF components in `idf/components/`:

- `arduino` (arduino-esp32 core, as an ESP-IDF component; pinned to 3.3.5)
- `ESPAsyncWebServer`
- `WiFiManager` (PBRunot fork; pinned to 0404abde...)
- `eModbus` (pinned to ed343224...)
- `ESPTelnet`
- `Uptime` (Uptime Library)

Each component must include its own `CMakeLists.txt` (or be adapted to one).

## Build

From the `idf/` directory:

```
. ~/esp-idf/export.sh
./scripts/fetch_components.sh
idf.py set-target esp32
idf.py build
```

## Flash

From the `idf/` directory:

```
./flash.sh /dev/ttyUSB0
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

If you want to reuse defaults from PlatformIO, translate `sdkconfig.dingtian` into `idf/sdkconfig.defaults`.

## Notes

- Ethernet setup currently relies on `ethernet_jl1101.*` and IDF ethernet APIs.
- RS485 uses Arduino `HardwareSerial`; keep `arduino` component enabled.

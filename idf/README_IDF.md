# ESP-IDF (v5.1.7) Build

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

The following libraries are expected as ESP-IDF components in `idf/components/`:

- `arduino` (arduino-esp32 core, as an ESP-IDF component; pinned to 3.0.7)
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

Or manually:

```
. ~/esp-idf/export.sh
idf.py -p /dev/ttyUSB0 flash
```

If you want to reuse defaults from PlatformIO, translate `sdkconfig.dingtian` into `idf/sdkconfig.defaults`.

## Notes

- Ethernet setup currently relies on `ethernet_jl1101.*` and IDF ethernet APIs.
- RS485 uses Arduino `HardwareSerial`; keep `arduino` component enabled.

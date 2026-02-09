#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-}"
if [[ -z "${PORT}" ]]; then
  echo "Usage: $0 /dev/ttyUSB0"
  exit 1
fi

python "${HOME}/esp-idf/components/esptool_py/esptool/esptool.py" \
  -p "${PORT}" -b 460800 --before default_reset --after hard_reset --chip esp32 write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x1000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xe000 build/ota_data_initial.bin \
  0x10000 build/heidelberg-1p3p.bin

#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-}"
if [[ -z "${PORT}" ]]; then
  echo "Usage: $0 /dev/ttyUSB0"
  exit 1
fi

BOOTLOADER_BIN="${SCRIPT_DIR}/build/bootloader/bootloader.bin"
PARTITION_BIN="${SCRIPT_DIR}/build/partition_table/partition-table.bin"
OTADATA_BIN="${SCRIPT_DIR}/build/ota_data_initial.bin"
APP_BIN="${SCRIPT_DIR}/build/heidelberg-1p3p.bin"

if [[ -f "${BOOTLOADER_BIN}" && -f "${PARTITION_BIN}" && -f "${OTADATA_BIN}" && -f "${APP_BIN}" ]]; then
  if command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL=(esptool.py)
  elif python3 -c "import esptool" >/dev/null 2>&1; then
    ESPTOOL=(python3 -m esptool)
  elif [[ -f "${HOME}/esp-idf/components/esptool_py/esptool/esptool.py" ]]; then
    ESPTOOL=(python3 "${HOME}/esp-idf/components/esptool_py/esptool/esptool.py")
  else
    echo "No esptool found. Install esptool or ESP-IDF." >&2
    exit 1
  fi

  "${ESPTOOL[@]}" \
    --chip esp32 \
    --port "${PORT}" \
    --baud 460800 \
    --before default_reset \
    --after hard_reset \
    write_flash \
    --flash_mode dio \
    --flash_size 4MB \
    --flash_freq 40m \
    0x1000 "${BOOTLOADER_BIN}" \
    0x8000 "${PARTITION_BIN}" \
    0xe000 "${OTADATA_BIN}" \
    0x10000 "${APP_BIN}"
else
  echo "Prebuilt binaries not found under ${SCRIPT_DIR}/build, falling back to idf.py flash."
  if ! command -v idf.py >/dev/null 2>&1; then
    echo "idf.py not found. Please run: . ~/esp-idf/export.sh" >&2
    exit 1
  fi
  IDF_VER="$(idf.py --version 2>/dev/null | sed -E 's/.*v([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
  if [[ -n "${IDF_VER}" ]]; then
    MIN_VER="5.3.0"
    if [[ "$(printf '%s\n' "${MIN_VER}" "${IDF_VER}" | sort -V | head -n1)" != "${MIN_VER}" ]]; then
      echo "Detected ESP-IDF ${IDF_VER}, but this project requires >= ${MIN_VER} for the idf/ path." >&2
      echo "Use a newer export, e.g.: . /tmp/esp-idf-5.5.2/export.sh" >&2
      exit 1
    fi
  fi
  idf.py -C "${SCRIPT_DIR}" -p "${PORT}" flash
fi

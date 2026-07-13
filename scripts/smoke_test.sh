#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_DIR="${ROOT_DIR}"

PORT=""
HOST=""
MODBUS_PORT="502"
UNIT_ID="1"
MONITOR_SECONDS="0"
SKIP_FETCH=0
SKIP_GUARD=0
SKIP_BUILD=0
SKIP_FLASH=0
SKIP_MODBUS=0

usage() {
  cat <<'EOF'
Usage: smoke_test.sh [options]

Options:
  --port <dev>            Flash target, e.g. /dev/ttyUSB0
  --host <ip-or-host>     Run Modbus TCP read checks against this host
  --modbus-port <port>    Modbus TCP port (default: 502)
  --unit-id <id>          Modbus unit/slave id (default: 1)
  --monitor-seconds <n>   Run serial monitor for n seconds after flash (default: 0)
  --skip-fetch            Skip ./scripts/fetch_components.sh
  --skip-guard            Skip check_no_arduino_types.sh
  --skip-build            Skip idf.py build
  --skip-flash            Skip flashing even when --port is set
  --skip-modbus           Skip Modbus checks even when --host is set
  -h, --help              Show this help

Examples:
  ./scripts/smoke_test.sh
  ./scripts/smoke_test.sh --port /dev/ttyUSB0 --monitor-seconds 20
  ./scripts/smoke_test.sh --host 192.168.1.42
  ./scripts/smoke_test.sh --port /dev/ttyUSB0 --host 192.168.1.42
EOF
}

log() {
  echo "[smoke] $*"
}

run() {
  log "$*"
  "$@"
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 2
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="${2:-}"; shift 2 ;;
    --host)
      HOST="${2:-}"; shift 2 ;;
    --modbus-port)
      MODBUS_PORT="${2:-}"; shift 2 ;;
    --unit-id)
      UNIT_ID="${2:-}"; shift 2 ;;
    --monitor-seconds)
      MONITOR_SECONDS="${2:-}"; shift 2 ;;
    --skip-fetch)
      SKIP_FETCH=1; shift ;;
    --skip-guard)
      SKIP_GUARD=1; shift ;;
    --skip-build)
      SKIP_BUILD=1; shift ;;
    --skip-flash)
      SKIP_FLASH=1; shift ;;
    --skip-modbus)
      SKIP_MODBUS=1; shift ;;
    -h|--help)
      usage; exit 0 ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 2 ;;
  esac
done

require_cmd bash
require_cmd git
require_cmd python3

if [[ -z "${IDF_PATH:-}" ]]; then
  if [[ -f "${HOME}/esp-idf/export.sh" ]]; then
    # shellcheck source=/dev/null
    . "${HOME}/esp-idf/export.sh" >/dev/null
  else
    echo "ESP-IDF not initialized. Run '. ~/esp-idf/export.sh' first." >&2
    exit 2
  fi
fi

require_cmd idf.py

if [[ "${SKIP_GUARD}" -eq 0 ]]; then
  run "${IDF_DIR}/scripts/check_no_arduino_types.sh"
fi

cd "${IDF_DIR}"

if [[ "${SKIP_FETCH}" -eq 0 ]]; then
  run ./scripts/fetch_components.sh
fi

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
  run idf.py build
fi

if [[ -n "${PORT}" && "${SKIP_FLASH}" -eq 0 ]]; then
  run idf.py -p "${PORT}" flash
  if [[ "${MONITOR_SECONDS}" -gt 0 ]]; then
    log "Starting monitor for ${MONITOR_SECONDS}s"
    if command -v timeout >/dev/null 2>&1; then
      set +e
      timeout "${MONITOR_SECONDS}"s idf.py -p "${PORT}" monitor
      rc=$?
      set -e
      if [[ "${rc}" -ne 0 && "${rc}" -ne 124 ]]; then
        exit "${rc}"
      fi
    else
      log "timeout not found; skipping monitor timeout run"
    fi
  fi
fi

if [[ -n "${HOST}" && "${SKIP_MODBUS}" -eq 0 ]]; then
  require_cmd mbpoll
  log "Running Modbus TCP read checks against ${HOST}:${MODBUS_PORT} (unit ${UNIT_ID})"
  run mbpoll -m tcp -a "${UNIT_ID}" -p "${MODBUS_PORT}" -0 -t 4:int -r 5 -c 1 "${HOST}"
  run mbpoll -m tcp -a "${UNIT_ID}" -p "${MODBUS_PORT}" -0 -t 4:int -r 10 -c 3 "${HOST}"
  run mbpoll -m tcp -a "${UNIT_ID}" -p "${MODBUS_PORT}" -0 -t 3:int -r 259 -c 1 "${HOST}"
  run mbpoll -m tcp -a "${UNIT_ID}" -p "${MODBUS_PORT}" -0 -t 3:int -r 261 -c 1 "${HOST}"
fi

log "Smoke test completed successfully."

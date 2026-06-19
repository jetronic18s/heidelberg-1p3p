#!/usr/bin/env bash
set -euo pipefail

HOST="192.168.178.51"
PORT="502"
UNIT_ID="1"
TIMEOUT_S="2"
WRITE_TEST=0
STRICT_LAYOUT_VERSION=0
PROBE_UNIT=1
ADDR_FLAG="-0"
BRIDGE_ONLY=0

usage() {
  cat <<'EOF'
Usage: ./scripts/modbus_regression.sh [options]

Options:
  --host <ip-or-host>    Target host (default: 192.168.178.51)
  --port <port>          Modbus TCP port (default: 502)
  --unit-id <id>         Modbus unit/slave id (default: 1)
  --timeout <seconds>    mbpoll timeout seconds (default: 2)
  --probe-unit           Probe common unit IDs and auto-select one (default: on)
  --no-probe-unit        Disable unit-ID probe (use --unit-id as-is)
  --bridge-only          Only test TCP reachability of the Modbus bridge (no RTU register checks)
  --write-test           Enable write/readback test for holding register 261
  --strict-layout        Fail if input register 4 (layout version) is unavailable
  -h, --help             Show help

Examples:
  ./scripts/modbus_regression.sh
  ./scripts/modbus_regression.sh --host 192.168.178.51 --unit-id 1
  ./scripts/modbus_regression.sh --host 192.168.178.51 --write-test
EOF
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 2
  fi
}

log() {
  echo "[modbus-regression] $*"
}

run_ok() {
  local name="$1"
  shift
  log "OK test: ${name}"
  if ! "$@"; then
    echo "FAILED: ${name}" >&2
    exit 1
  fi
}

run_fail() {
  local name="$1"
  shift
  log "NEG test (expect failure): ${name}"
  if "$@" >/tmp/modbus_neg.out 2>&1; then
    echo "FAILED: ${name} unexpectedly succeeded" >&2
    cat /tmp/modbus_neg.out >&2 || true
    exit 1
  fi
}

run_optional() {
  local name="$1"
  shift
  log "OPTIONAL test: ${name}"
  if ! "$@"; then
    if [[ "${STRICT_LAYOUT_VERSION}" -eq 1 ]]; then
      echo "FAILED (strict): ${name}" >&2
      exit 1
    fi
    echo "WARN: skipped optional test '${name}' (not supported on this device/firmware)." >&2
  fi
}

read_holding() {
  local reg="$1"
  local count="$2"
  mbpoll -m tcp -a "${UNIT_ID}" -p "${PORT}" ${ADDR_FLAG} -t 4 -r "${reg}" -c "${count}" -1 -o "${TIMEOUT_S}" "${HOST}"
}

read_input() {
  local reg="$1"
  local count="$2"
  mbpoll -m tcp -a "${UNIT_ID}" -p "${PORT}" ${ADDR_FLAG} -t 3 -r "${reg}" -c "${count}" -1 -o "${TIMEOUT_S}" "${HOST}"
}

write_holding() {
  local reg="$1"
  local value="$2"
  mbpoll -m tcp -a "${UNIT_ID}" -p "${PORT}" ${ADDR_FLAG} -t 4 -r "${reg}" "${value}" -1 -o "${TIMEOUT_S}" "${HOST}"
}

read_single_holding_value() {
  local reg="$1"
  local out
  out="$(read_holding "${reg}" 1)"
  echo "${out}" | awk '/^\[[[:space:]]*[0-9]+[[:space:]]*\]:/{gsub(/\[/,"",$1); gsub(/\]/,"",$1); print $2; exit}'
}

check_bridge_tcp() {
  if command -v nc >/dev/null 2>&1; then
    nc -z -w "${TIMEOUT_S}" "${HOST}" "${PORT}"
    return $?
  fi
  if command -v timeout >/dev/null 2>&1; then
    timeout "${TIMEOUT_S}" bash -c "cat < /dev/null > /dev/tcp/${HOST}/${PORT}" >/dev/null 2>&1
    return $?
  fi
  bash -c "cat < /dev/null > /dev/tcp/${HOST}/${PORT}" >/dev/null 2>&1
}

probe_unit_id() {
  local candidates=(1 2 3 4 5 6 7 8 9 10 247)
  local addr_modes=("-0" "")
  local mode_label=""
  for mode in "${addr_modes[@]}"; do
    for cand in "${candidates[@]}"; do
      if mbpoll -m tcp -a "${cand}" -p "${PORT}" ${mode} -t 4 -r 259 -c 1 -1 -o "${TIMEOUT_S}" "${HOST}" >/dev/null 2>&1; then
        UNIT_ID="${cand}"
        ADDR_FLAG="${mode}"
        if [[ -n "${ADDR_FLAG}" ]]; then
          mode_label="0-based (-0)"
        else
          mode_label="1-based (no -0)"
        fi
        log "Auto-detected reachable unit-id: ${UNIT_ID}, addressing mode: ${mode_label}"
        return 0
      fi
    done
  done
  return 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) HOST="${2:-}"; shift 2 ;;
    --port) PORT="${2:-}"; shift 2 ;;
    --unit-id) UNIT_ID="${2:-}"; shift 2 ;;
    --timeout) TIMEOUT_S="${2:-}"; shift 2 ;;
    --probe-unit) PROBE_UNIT=1; shift ;;
    --no-probe-unit) PROBE_UNIT=0; shift ;;
    --bridge-only) BRIDGE_ONLY=1; shift ;;
    --write-test) WRITE_TEST=1; shift ;;
    --strict-layout) STRICT_LAYOUT_VERSION=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

require_cmd mbpoll
require_cmd awk

if [[ "${BRIDGE_ONLY}" -eq 1 ]]; then
  log "Bridge-only mode: testing TCP reachability ${HOST}:${PORT}"
  if check_bridge_tcp; then
    log "Bridge TCP reachable."
    exit 0
  fi
  echo "FAILED: bridge TCP not reachable at ${HOST}:${PORT}" >&2
  exit 1
fi

if [[ "${PROBE_UNIT}" -eq 1 ]]; then
  if ! probe_unit_id; then
    echo "FAILED: no reachable Modbus unit-id found on ${HOST}:${PORT}. Try --unit-id <id> --no-probe-unit." >&2
    exit 1
  fi
fi

log "Target ${HOST}:${PORT} (unit ${UNIT_ID})"

# Positive checks (stable read-path)
run_optional "Read input reg 4 (layout version) x1" read_input 4 1
run_ok "Read input reg 5 x1" read_input 5 1
run_ok "Read input reg 10 x3" read_input 10 3
run_ok "Read holding reg 257 x1" read_holding 257 1
run_ok "Read holding reg 259 x1" read_holding 259 1
run_ok "Read holding reg 261 x1" read_holding 261 1
run_ok "Read holding reg 256 x7" read_holding 256 7

# Negative checks (should return Modbus exception/failed request)
run_fail "Read holding invalid address 65000 x1" read_holding 65000 1
run_fail "Read input invalid address 65000 x1" read_input 65000 1

if [[ "${WRITE_TEST}" -eq 1 ]]; then
  log "Write test enabled: register 261 readback roundtrip"
  orig="$(read_single_holding_value 261)"
  if [[ -z "${orig}" ]]; then
    echo "FAILED: could not parse current value from register 261" >&2
    exit 1
  fi
  log "Register 261 current value: ${orig}"
  run_ok "Write holding reg 261 with same value ${orig}" write_holding 261 "${orig}"
  after="$(read_single_holding_value 261)"
  if [[ "${after}" != "${orig}" ]]; then
    echo "FAILED: write/readback mismatch on reg 261 (${orig} != ${after})" >&2
    exit 1
  fi
fi

log "Regression checks passed."

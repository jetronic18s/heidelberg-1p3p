#!/usr/bin/env bash
# Run the evcc test bench: wallbox simulator plus a local evcc instance.
#
#   bench/start.sh --init          create the bench directory and evcc.yaml
#   bench/start.sh                 run in the foreground (Ctrl+C stops both)
#   bench/start.sh --background    run in the background
#   bench/start.sh --stop          stop a background run
#
# The bench directory (default: ../evcc_testbench next to the repository,
# override with BENCH_DIR) holds the evcc binary, bench.env, the generated
# evcc.yaml, the database and the logs. See bench/README.md.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_DIR="${BENCH_DIR:-$(cd "$REPO/.." && pwd)/evcc_testbench}"
ENV_FILE="$BENCH_DIR/bench.env"

# Settings: bench.env, overridable by the environment.
ESP_IP_ENV="${ESP_IP:-}" RS485_ENV="${RS485:-}"
EVCC_PORT_ENV="${EVCC_PORT:-}" SIM_PORT_ENV="${SIM_PORT:-}"
if [[ -f "$ENV_FILE" ]]; then
  # shellcheck source=/dev/null
  source "$ENV_FILE"
fi
ESP_IP="${ESP_IP_ENV:-${ESP_IP:-}}"
RS485="${RS485_ENV:-${RS485:-/dev/ttyUSB0}}"
EVCC_PORT="${EVCC_PORT_ENV:-${EVCC_PORT:-7071}}"
SIM_PORT="${SIM_PORT_ENV:-${SIM_PORT:-8502}}"

init() {
  mkdir -p "$BENCH_DIR"
  if [[ ! -f "$ENV_FILE" ]]; then
    if [[ -z "$ESP_IP" ]]; then
      echo "ESP_IP is not set, e.g.: ESP_IP=192.168.178.51 $0 --init" >&2
      exit 1
    fi
    cat > "$ENV_FILE" <<EOF
# evcc test bench settings, read by bench/start.sh and bench/bench_test.py
ESP_IP=$ESP_IP
RS485=$RS485
EVCC_PORT=$EVCC_PORT
SIM_PORT=$SIM_PORT
EOF
    echo "created $ENV_FILE"
  fi
  sed -e "s|{{ESP_IP}}|$ESP_IP|g" -e "s|{{EVCC_PORT}}|$EVCC_PORT|g" \
      -e "s|{{BENCH_DIR}}|$BENCH_DIR|g" \
      "$REPO/bench/evcc.yaml.template" > "$BENCH_DIR/evcc.yaml"
  echo "created $BENCH_DIR/evcc.yaml for the board at $ESP_IP"
  if [[ ! -x "$BENCH_DIR/evcc" ]]; then
    echo "missing: $BENCH_DIR/evcc (the patched evcc binary, see bench/README.md)"
  fi
}

stop() {
  local pid
  for name in evcc sim; do
    if [[ -f "$BENCH_DIR/$name.pid" ]]; then
      pid="$(cat "$BENCH_DIR/$name.pid")"
      kill "$pid" 2>/dev/null && echo "stopped $name ($pid)" || true
      rm -f "$BENCH_DIR/$name.pid"
    fi
  done
}

run() {
  for f in evcc evcc.yaml; do
    if [[ ! -e "$BENCH_DIR/$f" ]]; then
      echo "missing $BENCH_DIR/$f, run $0 --init first (see bench/README.md)" >&2
      exit 1
    fi
  done
  stop >/dev/null

  echo 0 > "$BENCH_DIR/grid_power"
  : > "$BENCH_DIR/sim.log"
  python3 -I "$REPO/scripts/heidelberg_sim.py" \
    --port "$RS485" --esp-url "http://$ESP_IP/" --control-port "$SIM_PORT" \
    --grid-file "$BENCH_DIR/grid_power" --pv 0 --house 500 \
    >> "$BENCH_DIR/sim.log" 2>&1 < /dev/null &
  echo $! > "$BENCH_DIR/sim.pid"
  echo "simulator running, log: $BENCH_DIR/sim.log"

  # The simulated wallbox stays silent for its boot delay after power-on;
  # evcc gives up on a charger whose first Modbus writes fail and retries
  # only after 15 minutes.
  echo "waiting for the simulated wallbox..."
  for _ in $(seq 60); do
    if echo json | nc -q1 localhost "$SIM_PORT" 2>/dev/null | grep -q '"answering": true'; then
      break
    fi
    sleep 1
  done

  echo "evcc UI: http://127.0.0.1:$EVCC_PORT, log: $BENCH_DIR/evcc.log"
  "$BENCH_DIR/evcc" --config "$BENCH_DIR/evcc.yaml" --database "$BENCH_DIR/evcc.db" \
    --disable-auth --disable-onboarding >> "$BENCH_DIR/evcc.log" 2>&1 &
  echo $! > "$BENCH_DIR/evcc.pid"
}

case "${1:-}" in
  --init) init ;;
  --stop) stop ;;
  --background) run ;;
  "")
    run
    tail -n 0 -f "$BENCH_DIR/evcc.log" &
    TAIL_PID=$!
    trap 'stop; kill $TAIL_PID 2>/dev/null' EXIT
    wait "$(cat "$BENCH_DIR/evcc.pid")"
    ;;
  *) sed -n '2,12p' "$0"; exit 1 ;;
esac

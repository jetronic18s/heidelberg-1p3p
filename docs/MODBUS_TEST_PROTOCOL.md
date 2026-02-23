# Modbus Test Protocol (With Wallbox)

This checklist is intended for validation runs against a real device setup
(bridge + connected Heidelberg wallbox).

## Preconditions

- Firmware running and reachable via IP (example: `192.168.178.51`)
- Wallbox connected via RS485/RTU to the ESP32 bridge
- Modbus enabled in config
- Optional for extended debugging: telnet debug enabled (port 23)

## 1. Baseline Reachability

Command:

```bash
./scripts/modbus_regression.sh --host 192.168.178.51 --bridge-only
```

Expected:

- Script exits with `Bridge TCP reachable.`

## 2. Modbus Regression Read Path

Command:

```bash
./scripts/modbus_regression.sh --host 192.168.178.51
```

Expected:

- Input/Holding register reads succeed
- Invalid-address checks fail as expected
- Script exits with `Regression checks passed.`

## 3. Modbus Write/Readback (Safe)

Command:

```bash
./scripts/modbus_regression.sh --host 192.168.178.51 --write-test
```

Expected:

- Register `261` write/readback roundtrip succeeds (same-value write)
- Script exits with `Regression checks passed.`

## 4. Parallel Stress (Bridge Robustness)

Recommended first run:

```bash
./scripts/modbus_stress.py --host 192.168.178.51 --clients 6 --duration-s 120 --max-transport-errors 5
```

Expected:

- Non-zero `normal_ok`
- `invalid_unexpected_ok: 0`
- No fail summary at script end

## 5. Functional Switch Sequence Under Polling

While continuous polling/stress is active:

- Trigger `1P` and `3P` switch via web UI (or your normal control path)
- Observe status page + debug output

Expected:

- Sequence completes without deadlock/hang
- Bridge remains responsive during/after switching
- Register values remain coherent (no persistent exception storm)

## 6. Network Resilience

Test with active polling:

- Pull LAN cable for a short interval and reconnect

Expected:

- Device recovers network path
- Modbus bridge becomes reachable again
- No reboot loop or permanent bridge lockup

## 7. Test Log (fill in)

- Date:
- Firmware commit:
- Device IP:
- Regression result:
- Write-test result:
- Stress result:
- Switch-sequence result:
- LAN flap result:
- Notes / anomalies:

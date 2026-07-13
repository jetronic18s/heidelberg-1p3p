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

## 6. Manual mbpoll Checks

Use these commands for quick checks during bring-up. Replace the IP address if
the ESP32 bridge uses a different address.

Read the virtual phase switch register exposed for evcc:

```bash
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 4 -r 4 -c 1 192.168.178.3
```

Expected:

- `1` when the controller is in 1-phase mode
- `3` when the controller is in 3-phase mode

Read the Heidelberg register layout version:

```bash
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 3 -r 4 -c 1 192.168.178.3
```

Expected:

- `264` (`0x108`)

Read charging state:

```bash
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 3 -r 5 -c 1 192.168.178.3
```

Read phase voltages:

```bash
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 3 -r 10 -c 3 192.168.178.3
```

Read relevant holding registers:

```bash
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 4 -r 257 -c 1 192.168.178.3
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 4 -r 259 -c 1 192.168.178.3
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 4 -r 261 -c 1 192.168.178.3
```

Write the virtual phase switch register manually:

```bash
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 4 -r 4 192.168.178.3 1
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 4 -r 4 192.168.178.3 3
```

Expected:

- Web status moves through the switch sequence.
- A following read of holding register `4` returns the requested phase count.
- No safety fault is shown on the status page.

## 7. evcc Parallel Test

Start evcc and verify that the loadpoint UI offers 1P/3P phase selection. Then
keep evcc running and run:

```bash
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 4 -r 4 -c 1 192.168.178.3
```

Expected:

- The command still returns `1` or `3` while evcc is connected.
- No persistent timeouts occur.
- The web status shows bridge clients/messages increasing.

## 8. Auto Test

Preconditions:

- evcc is running and can select 1P/3P in the loadpoint UI.
- Vehicle is connected.
- Charging can be enabled safely.
- Phase switch delay is configured as desired.

Test sequence:

1. Start with a known phase mode, preferably 1P.
2. Start charging via evcc.
3. Trigger a switch to 3P via evcc.
4. Observe the web status page.
5. After the delay completed, read holding register `4`.
6. Repeat in the other direction, 3P to 1P.

Expected web status sequence:

- `SwitchPhases`
- `WaitingForZero`
- `WaitingForOff`
- `ConfirmedOff`
- `SwitchedOn`
- `Delay`
- `Running`

Additional checks after each switch:

```bash
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 4 -r 4 -c 1 192.168.178.3
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 3 -r 5 -c 1 192.168.178.3
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 3 -r 6 -c 3 192.168.178.3
mbpoll -m tcp -a 1 -p 502 -0 -o 2 -t 3 -r 10 -c 3 192.168.178.3
```

Expected:

- Holding register `4` matches the requested phase mode.
- No safety fault is present.
- evcc remains connected.
- Charging resumes after the configured switch delay.

## 9. Network Resilience

Test with active polling:

- Pull LAN cable for a short interval and reconnect

Expected:

- Device recovers network path
- Modbus bridge becomes reachable again
- No reboot loop or permanent bridge lockup

## 10. Test Log (fill in)

- Date:
- Firmware commit:
- Device IP:
- Regression result:
- Write-test result:
- Stress result:
- Switch-sequence result:
- LAN flap result:
- Notes / anomalies:

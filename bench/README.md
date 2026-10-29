# Test bench

Bench setup to test the firmware, and new evcc versions, without a real
wallbox:

```
evcc (local, port 7071) ──Modbus TCP──▶ ESP32 bench board ──RS485──▶ USB-RS485 adapter
        ▲                                     │ relays                    │
        │ grid power                          ▼                           ▼
        └──────────── grid_power ◀──── heidelberg_sim.py (simulated Heidelberg + house)
```

- **Bench board:** a Dingtian DT-R002 with the firmware under test.
- **Simulator** (`scripts/heidelberg_sim.py`): answers Modbus RTU like a
  Heidelberg Energy Control (register layout 0x108), with a car and a simple
  house model (PV, house load, grid power).
- **evcc:** a local instance of the patched evcc with Heidelberg phase
  switching, with its own config, port and database.
- **Tests** (`bench/bench_test.py`): drive all three and check the results.

Typical uses:

- **Before a firmware release:** flash the bench board, run the tests.
- **Before an evcc update:** put the new evcc build into the bench directory,
  run the tests, then update the production instance.
- **Developing a change:** reproduce a situation by hand with the simulator
  commands and watch evcc react.

## Files

In the repository:

| File | Purpose |
|---|---|
| `bench/start.sh` | creates the bench directory, starts and stops simulator and evcc |
| `bench/evcc.yaml.template` | evcc config template, filled in by `start.sh --init` |
| `bench/bench_test.py` | automated tests |
| `scripts/heidelberg_sim.py` | wallbox simulator |

In the bench directory, outside the repository (default `../evcc_testbench`
next to the repository, override with `BENCH_DIR`):

| File | Purpose |
|---|---|
| `evcc` | patched evcc binary (not versioned, see below) |
| `bench.env` | local settings: board IP, RS485 adapter, ports |
| `evcc.yaml` | generated evcc config |
| `evcc.db` | evcc database of the bench |
| `evcc.log`, `sim.log` | logs of evcc and the simulator |
| `grid_power` | grid power in W, written by the simulator, read by evcc |
| `evcc.pid`, `sim.pid` | process ids of a background run |

## Hardware

- **Bench board** with the firmware, reachable on the network. Ethernet is
  recommended: WiFi on a weak signal makes the web page slow after a reboot.
- **USB-RS485 adapter** on the PC (e.g. FTDI, `/dev/ttyUSB0`). Connect A to
  A and B to B on the board's RS485 terminals, GND if available. Swap A and B
  if the simulator never logs a request.
- **Contactor feedback through the board's own relays.** Each relay plays a
  contactor, its normally closed contact plays the auxiliary contact:

  | Relay | COM | NC | NO |
  |---|---|---|---|
  | Relay 1 (1p, GPIO16) | GND | DI1 | – |
  | Relay 2 (3p, GPIO2) | GND | DI2 | – |

  Relay off closes NC and pulls the input to GND ("contactor off"); relay on
  opens NC ("contactor on"). If DI1 and DI2 are swapped, the board reports
  `0xE102` after the first switch-on; swap the NC wires.
- **Switchable plug (optional)** for the board's power supply: a Tasmota
  plug, set as `POWER_SWITCH=http://<plug IP>` in `bench.env`. The power loss
  tests use it; without it they are skipped. The tests switch it through the
  Tasmota web UI (`/?m=1`), no Tasmota configuration is needed; a web
  password is not supported.
- **Disconnect the USB serial flasher** during tests: on Dingtian boards
  RS485 shares UART0 with it. Use Telnet debug for logs (web UI → Config).

The simulator reads the start page of the board to see which contactor
feedback is active, and treats the wallbox as powered with 1 or 3 phases
while it is. A wallbox without power does not answer, like the real one.

Recommended board settings for the bench: a short phase switch delay
(e.g. 3 s instead of 20 s) makes switches faster. The simulated wallbox boots
in 5 s instead of about 20 s.

## Setup

1. **evcc binary:** build the patched evcc (branch `heidelberg-1p3p-0.318.1`
   of [jetronic18s/evcc](https://github.com/jetronic18s/evcc/tree/heidelberg-1p3p-0.318.1)) and put
   it into the bench directory. With a build that has the sponsorship checks
   removed no token is needed; otherwise add the sponsor token to the config.

   ```bash
   cd <evcc fork>
   CGO_ENABLED=0 go build -tags=release -trimpath \
     -ldflags "-X github.com/evcc-io/evcc/util.Version=<version>-test -s -w" \
     -o ../evcc_testbench/evcc .
   ```

2. **Bench directory and config:**

   ```bash
   ESP_IP=192.168.178.51 bench/start.sh --init
   ```

   This creates `bench.env` and `evcc.yaml`. Edit `bench.env` for another
   adapter or ports, or set `POWER_SWITCH` for the power loss tests, then run `--init` again to regenerate `evcc.yaml`.
   **The config must only ever point at the bench board.**

## Running the bench

```bash
bench/start.sh                # foreground, Ctrl+C stops simulator and evcc
bench/start.sh --background   # background
bench/start.sh --stop         # stop a background run
```

`start.sh` waits until the simulated wallbox answers before it starts evcc:
evcc gives up on a charger whose first Modbus writes fail and retries only
after 15 minutes.

- evcc UI: http://127.0.0.1:7071 (no login)
- simulator commands: `echo <command> | nc -q1 localhost 8502`

### Simulator commands

| Command | Effect |
|---|---|
| `car on` / `car off` | plug in / unplug the car |
| `pv <W>` | set the PV power |
| `house <W>` | set the house load (start: 500 W) |
| `silent on` / `silent off` | the wallbox stops answering (e.g. for `0xE104`) |
| `phases auto\|0\|1\|3` | power from the contactor feedback (auto) or forced |
| `status` | write the state to `sim.log` |
| `json` | reply with the state as JSON (phases, status, currents, energy, watchdog, ...) |
| `quit` | stop the simulator |

Each command is answered with one line: `ok`, JSON, or an error.

### Examples

PV charging and an automatic 3p → 1p switch:

```bash
echo "car on"  | nc -q1 localhost 8502   # evcc enables charging after the enable delay
echo "pv 6000" | nc -q1 localhost 8502   # 5.5 kW surplus: 3p at about 8 A
echo "pv 3000" | nc -q1 localhost 8502   # 2.5 kW: below the 3p minimum, evcc switches to 1p
echo json      | nc -q1 localhost 8502   # 1p at about 11 A, grid close to 0 W
```

Restart of the board while charging (the energy counter must not go back):

```bash
curl -X POST http://192.168.178.51/reboot
grep -v " rx " ../evcc_testbench/sim.log   # "locked, energy counter saved" before "power off"
```

Dead wallbox (safety fault `0xE104` about 60 s after switch-on):

```bash
echo "silent on" | nc -q1 localhost 8502
curl -X POST http://192.168.178.51/reboot
# afterwards: silent off, reboot again
```

Evaluating logs:

```bash
grep -E "switched phases|charger status|set charge current|ERROR|WARN" ../evcc_testbench/evcc.log
grep -v " rx " ../evcc_testbench/sim.log
```

## Automated tests

With the bench running:

```bash
bench/bench_test.py             # all tests, about 45-60 minutes
bench/bench_test.py 3 6         # selected tests
bench/bench_test.py --list      # list the tests
bench/bench_test.py --verbose   # log every check that passes
bench/bench_test.py --soak 60   # soak test for 60 minutes instead
```

Test 10 uploads a firmware image via OTA: by default `build/heidelberg-1p3p.bin`,
otherwise set `FIRMWARE_BIN`. Use the image that is on the board, so the test
does not change the firmware under test.

Before a release, run all tests once on LAN and once on WiFi with the release
candidate on the board, and keep both outputs in the bench directory, e.g.
`bench/bench_test.py > ../evcc_testbench/results/v1.1.3-lan.out` and
`-wlan.out` (with `ESP_IP` of the WiFi address).

Before each test the script brings the bench into a common start state: car
unplugged, no PV, 500 W house load, evcc in PV mode with automatic phases,
enable and disable delay 30 s. Afterwards it sets the delays back to 60 s.
Each test waits for the expected state with a timeout and fails with a
description if it does not come. "Charging on N phases" means evcc reports
charging, the board is back in `Running` and current flows on exactly N
phases.

Checks after every test:

- **evcc log:** no errors or warnings, except the ones expected while the
  board or evcc restarts (connection errors, `exception '11'`,
  `charger out of sync`). `invalid status`, `exception '4'` and
  `phases mismatch` always fail a test, except where a test expects them
  (e.g. one `exception '4'` while the board runs on for a moment on its
  capacitors after a power loss).
- **Energy counter:** the simulator counts energy lost when the wallbox loses
  power without being locked first. Any loss fails the test, except in the
  tests with a wallbox that does not answer (it cannot be locked).
- **Safety faults:** a fault on the board fails the test, except in the tests
  that provoke one.

Results:

| Result | Meaning |
|---|---|
| `PASS` | test passed |
| `FAIL` | test failed: the reason is printed below the test |
| `KNOWN` | test failed because of a known, open problem (see below) |
| `FIXED?` | a test marked as known problem passed: check and remove the mark |
| `SKIP` | test skipped, e.g. a power loss test without `POWER_SWITCH` |

The exit code is 0 if no test failed; `KNOWN` does not count as failure. A
failure while preparing a test is always `FAIL`, also for tests marked as
known problem.

| # | Test | Checks |
|---|---|---|
| 1 | Boot | board reaches Running, evcc sees a charger with phase switching |
| 2 | PV charging | car + 6 kW PV: evcc charges 3p at 6.5–10 A, grid within ±600 W |
| 3 | Automatic 3p → 1p | 3 kW PV: evcc switches to 1p, 9–13 A on L1 |
| 4 | Automatic 1p → 3p | 7 kW PV: evcc switches back to 3p |
| 5 | Manual switch | evcc phases 1, then 3: the board switches each time |
| 6 | Current after 3p → 1p | 5 kW PV, switch to 1p: the box keeps evcc's 16 A instead of 6 A (fix `b1ff5b1`) |
| 7 | Switch without evcc | switch via register 4 while charging above 7 A: restart current at most 6 A |
| 8 | Switch while off | evcc mode off, switch: the box stays at 0 A, charges again after mode PV |
| 9 | Restart while charging | no car disconnect in evcc, no evcc errors, charging resumes |
| 10 | OTA while charging | firmware upload while charging: evcc gets cached values during the upload, then like a restart |
| 11 | Restart without car | no phantom "car connected" in evcc |
| 12 | Restart on 3p | the board boots on 3p again, no phases mismatch in evcc after the next car connect |
| 13 | Unplug while charging | evcc sees the car leave, current drops to 0 |
| 14 | Unplug during a restart | evcc sees the car leave after the restart |
| 15 | Plug in during a switch | evcc sees the car after the switch and charges |
| 16 | Two switches in a row | 1p and right after it 3p: the second request is kept and run after the first switch, board and evcc agree |
| 17 | House load spike | +1.5 kW house load: evcc lowers the current and raises it again |
| 18 | Mode off | evcc stops charging, wallbox max current 0 |
| 19 | evcc restart | evcc process restarted while charging: takes over without errors |
| 20 | evcc gone | evcc stopped while charging: within 30 s the board applies the failsafe current (0 A) and shows `[evcc timeout]`; charging resumes when evcc is back |
| 21 | Wallbox dies while charging | silent wallbox while charging: its watchdog stops charging, the board shows `[wallbox not responding]` after 60 s, charging resumes without reboot when it answers again, the status page lists the outage |
| 22 | Dead wallbox | silent wallbox after power-on: safety fault `0xE104`, recovery after a reboot |
| 23 | Power loss while charging | board and simulated wallbox off for 15 s while charging on 3p: the status page reports a power-on restart, evcc charges again (`exception '11'` until the wallbox answers, `phases mismatch` once because the board boots on 1p) |
| 24 | Power loss without car | the same without a car: no phantom car connect in evcc |
| 25 | Disable after a switch | evcc switches to 1p and disables charging right after it: the current stays at 0 A |

### Known problems

None at the moment. Mark a test that fails because of a known, open problem
with `@known_issue("...")` in `bench/bench_test.py`; it is then reported as
`KNOWN` until the problem is fixed.

### Soak test

`bench/bench_test.py --soak <minutes>` plugs in the car and moves PV along a
sine curve between 1 and 8 kW with a 10 minute period, which causes a phase
switch every few minutes. It fails on a safety fault, a lost energy counter
or unexpected evcc log lines, and prints the number of phase switches.

### Manual checks

Not automated, because they need a hand on the hardware:

- **Power loss** without a switchable plug: unplug the board's power supply
  while charging and plug it back in, as in tests 23 and 24.
- **Contactor faults:** pull the NC wire of DI1 while running (`0xE103`, or
  `0xE101` with the other contactor on); tie DI1 to GND permanently (`0xE102`
  after switch-on).
- **Phase switch from the board's web UI:** switches without evcc writing a
  current; the board restarts at most at 6 A.

## Limits of the simulation

- The simulated car follows the current limit at once, minus 0.2 A, and
  starts 2 s after the wallbox allows charging. Real cars ramp and may sleep.
- The relays switch instantly; real contactors can drop out with a delay.
- The house model is ideal: no battery, no delays. Real sites react slower
  (e.g. a home battery overshooting after a switch).
- Register behaviour follows the Amperfied register description for layout
  0x108 (261 back to 0 after power-on, 257-259 and 262 kept; watchdog: without
  a Modbus request for register 257 ms the failsafe current from 262 applies),
  and observations on a real Heidelberg: status 10 while locked and for a few
  seconds after the unlock, energy counter saved on lock and lost on power
  loss. The status 1 and odd voltages a real Heidelberg reports for a moment
  right after power-on are not simulated.

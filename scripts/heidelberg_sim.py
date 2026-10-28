#!/usr/bin/env python3
"""Simulate a Heidelberg Energy Control wallbox on a USB-RS485 adapter.

Answers Modbus RTU requests from the ESP32 like a wallbox with register
layout 0x108. The wallbox is powered while a contactor feedback input of the
ESP32 is active: the simulator polls the ESP32 start page and reads its
"1P"/"3P"/"~P" prefix. Without --esp-url it is always powered (1p).

Behaviour:
- After power-on it stays silent for --boot-delay seconds. Register 261
  falls back to 0, registers 257-259 and 262 keep their values (layout 0x108,
  see the Amperfied register description). While 259 is 0 it reports status 10.
- After an unlock it still reports status 10 for --unlock-delay seconds.
- A connected car charges with min(register 261, --car-max) on the powered
  phases while unlocked and register 261 >= 6 A.
- The energy counter is saved when the wallbox gets locked; a power loss
  falls back to the saved value, like the real Heidelberg.

Commands on stdin or on the control port (e.g. `echo car on | nc localhost 8502`):
  car on|off        connect or disconnect the car
  silent on|off     stop answering (simulates a dead wallbox)
  phases auto|1|3|0 power from the ESP32 feedback, or force 1p/3p/off
  pv <W> / house <W> set PV power and house load for --grid-file
  status            print the current state
  quit              stop the simulator
"""
import argparse
import os
import re
import socket
import sys
import threading
import time
import urllib.request

import serial

LAYOUT_VERSION = 0x108
STATUS_A1, STATUS_B1, STATUS_B2, STATUS_C2, STATUS_LOCKED = 2, 4, 5, 7, 10
MIN_CURRENT = 60  # 0.1 A
INPUT_REGS = list(range(0, 20)) + [100, 101]
HOLDING_REGS = range(256, 266)


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(pdu):
    return pdu + crc16(pdu).to_bytes(2, "little")


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


class Wallbox:
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.car = args.car
        self.silent = False
        self.forced_phases = None if args.esp_url else 1
        self.esp_phases = 0
        self.powered_phases = 0
        self.boot_until = 0.0
        self.holding = {reg: 0 for reg in HOLDING_REGS}
        self.holding.update({257: 15000, 259: 1})
        self.unlocked_at = time.monotonic() - args.unlock_delay
        self.charge_allowed_at = None
        self.energy_wh = float(args.energy)
        self.saved_energy_wh = float(args.energy)
        self.energy_power_on_wh = 0.0
        self.last_tick = time.monotonic()
        self.last_status = None
        self.pv_w = float(args.pv)
        self.house_w = float(args.house)
        self.power_w = 0.0

    # state

    def phases(self):
        return self.forced_phases if self.forced_phases is not None else self.esp_phases

    def update(self):
        now = time.monotonic()
        dt = now - self.last_tick
        self.last_tick = now
        phases = self.phases()
        if phases != self.powered_phases:
            if self.powered_phases and not phases:
                log(f"power off, energy counter falls back to {self.saved_energy_wh:.0f} Wh")
                self.energy_wh = self.saved_energy_wh
            if not self.powered_phases and phases:
                log(f"power on ({phases}p), answering in {self.args.boot_delay:.0f} s")
                self.boot_until = now + self.args.boot_delay
                self.holding[261] = 0
                self.unlocked_at = None if self.holding[259] == 0 else now - self.args.unlock_delay
                self.charge_allowed_at = None
                self.energy_power_on_wh = 0.0
            elif phases:
                log(f"phases changed to {phases}p")
            self.powered_phases = phases
        currents = self.currents(now)
        power = sum(c * 230 for c in currents) / 10
        self.power_w = power
        self.energy_wh += power * dt / 3600
        self.energy_power_on_wh += power * dt / 3600
        status = self.status(now)
        if status != self.last_status:
            log(f"status {self.last_status} -> {status}")
            self.last_status = status

    def locked(self, now):
        if self.holding[259] == 0 or self.unlocked_at is None:
            return True
        return now - self.unlocked_at < self.args.unlock_delay

    def charging(self, now):
        if not self.car or self.locked(now) or self.holding[261] < MIN_CURRENT:
            self.charge_allowed_at = None
            return False
        if self.charge_allowed_at is None:
            self.charge_allowed_at = now
        return now - self.charge_allowed_at >= self.args.car_start_delay

    def status(self, now):
        if self.locked(now):
            return STATUS_LOCKED
        if not self.car:
            return STATUS_A1
        if self.charging(now):
            return STATUS_C2
        return STATUS_B2 if self.charge_allowed_at is not None else STATUS_B1

    def currents(self, now):
        if not self.powered_phases or not self.charging(now):
            return [0, 0, 0]
        current = max(min(self.holding[261], self.args.car_max * 10) - 2, 0)
        return [current] * self.powered_phases + [0] * (3 - self.powered_phases)

    def input_registers(self):
        now = time.monotonic()
        regs = {reg: 0 for reg in INPUT_REGS}
        currents = self.currents(now)
        voltages = [230 if i < self.powered_phases else 3 for i in range(3)]
        energy = int(self.energy_wh)
        energy_on = int(self.energy_power_on_wh)
        regs.update({
            4: LAYOUT_VERSION,
            5: self.status(now),
            6: currents[0], 7: currents[1], 8: currents[2],
            9: 250,
            10: voltages[0], 11: voltages[1], 12: voltages[2],
            13: 1,
            14: int(sum(c * v for c, v in zip(currents, voltages)) / 10),
            15: energy_on >> 16, 16: energy_on & 0xFFFF,
            17: energy >> 16, 18: energy & 0xFFFF,
            100: int(self.args.car_max), 101: 6,
        })
        return regs

    def write_holding(self, reg, value):
        old = self.holding[reg]
        self.holding[reg] = value
        if reg == 259:
            if value == 0 and old != 0:
                self.saved_energy_wh = self.energy_wh
                self.unlocked_at = None
                log(f"locked, energy counter saved at {self.saved_energy_wh:.0f} Wh")
            elif value != 0 and old == 0:
                self.unlocked_at = time.monotonic()
                log("unlocked")
        if reg == 261 and value != old:
            log(f"max current {value / 10:.1f} A")

    def grid_w(self):
        return self.house_w + self.power_w - self.pv_w

    def answering(self):
        return (self.powered_phases and not self.silent
                and time.monotonic() >= self.boot_until)

    # modbus

    def handle(self, frame):
        """Return the response PDU for a request frame, or None to stay silent."""
        if len(frame) < 4 or crc16(frame[:-2]) != int.from_bytes(frame[-2:], "little"):
            return None
        if frame[0] != self.args.slave:
            return None
        with self.lock:
            self.update()
            if not self.answering():
                return None
            return self.handle_pdu(frame[0], frame[1:-2])

    def handle_pdu(self, slave, pdu):
        fc = pdu[0]

        def error(code):
            return bytes([slave, fc | 0x80, code])

        if fc in (3, 4) and len(pdu) == 5:
            addr = int.from_bytes(pdu[1:3], "big")
            count = int.from_bytes(pdu[3:5], "big")
            regs = self.input_registers() if fc == 4 else self.holding
            if count == 0 or any(a not in regs for a in range(addr, addr + count)):
                return error(2)
            data = b"".join(regs[a].to_bytes(2, "big") for a in range(addr, addr + count))
            return bytes([slave, fc, len(data)]) + data
        if fc == 6 and len(pdu) == 5:
            addr = int.from_bytes(pdu[1:3], "big")
            if addr not in self.holding:
                return error(2)
            self.write_holding(addr, int.from_bytes(pdu[3:5], "big"))
            return bytes([slave]) + pdu
        if fc == 16 and len(pdu) >= 6:
            addr = int.from_bytes(pdu[1:3], "big")
            count = int.from_bytes(pdu[3:5], "big")
            values = pdu[6:6 + 2 * count]
            if len(values) != 2 * count or any(a not in self.holding for a in range(addr, addr + count)):
                return error(2)
            for i in range(count):
                self.write_holding(addr + i, int.from_bytes(values[2 * i:2 * i + 2], "big"))
            return bytes([slave]) + pdu[:5]
        return error(1)

    # control

    def command(self, line):
        words = line.split()
        with self.lock:
            if words[:1] == ["car"] and words[1:] in (["on"], ["off"]):
                self.car = words[1] == "on"
                log(f"car {'connected' if self.car else 'disconnected'}")
            elif words[:1] == ["silent"] and words[1:] in (["on"], ["off"]):
                self.silent = words[1] == "on"
                log(f"silent {words[1]}")
            elif words[:1] == ["phases"] and words[1:] in (["auto"], ["0"], ["1"], ["3"]):
                self.forced_phases = None if words[1] == "auto" else int(words[1])
                log(f"phases {words[1]}")
            elif words[:1] in (["pv"], ["house"]) and len(words) == 2 and words[1].lstrip("-").isdigit():
                setattr(self, f"{words[0]}_w", float(words[1]))
                log(f"{words[0]} {words[1]} W")
            elif words == ["status"]:
                self.update()
                regs = self.input_registers()
                log(f"powered={self.powered_phases}p answering={bool(self.answering())} "
                    f"car={self.car} status={regs[5]} lock={self.holding[259]} "
                    f"max={self.holding[261] / 10:.1f}A I={[regs[i] / 10 for i in (6, 7, 8)]} "
                    f"energy={self.energy_wh:.0f}Wh saved={self.saved_energy_wh:.0f}Wh "
                    f"grid={self.grid_w():.0f}W (house {self.house_w:.0f} + car {self.power_w:.0f} - pv {self.pv_w:.0f})")
            elif words == ["quit"]:
                return False
            elif words:
                log(f"unknown command: {line.strip()}")
        return True


def request_length(buf):
    """Length of the Modbus RTU request at the start of buf, None if unknown yet."""
    if len(buf) < 2:
        return None
    if buf[1] in (3, 4, 6):
        return 8
    if buf[1] == 16:
        return 9 + buf[6] if len(buf) >= 7 else None
    return 0  # unsupported function code: resync


def serve_rtu(box, port):
    # USB adapters deliver bytes in bursts (FTDI: up to 16 ms), so frames are
    # split by their length instead of by the 3.5 character gap.
    ser = serial.Serial(port, 19200, bytesize=8, parity=serial.PARITY_EVEN,
                        stopbits=1, timeout=0.01)
    buf = bytearray()
    last_rx = 0.0
    last_tx = b""
    while True:
        chunk = ser.read(256)
        now = time.monotonic()
        if chunk:
            buf += chunk
            last_rx = now
        elif buf and now - last_rx > 0.2:
            if box.args.verbose:
                log(f"rx {bytes(buf).hex(' ')} -> dropped")
            buf = bytearray()
        if last_tx and buf.startswith(last_tx):
            # Some adapters echo their own transmission.
            del buf[:len(last_tx)]
            last_tx = b""
        while True:
            length = request_length(buf)
            if length is None or len(buf) < length:
                break
            frame = bytes(buf[:length])
            if length == 0 or crc16(frame[:-2]) != int.from_bytes(frame[-2:], "little"):
                del buf[0]  # resync on garbage
                continue
            del buf[:length]
            response = box.handle(frame)
            if box.args.verbose:
                answer = "tx " + with_crc(response).hex(" ") if response else "no answer"
                log(f"rx {frame.hex(' ')} -> {answer}")
            if response is not None:
                last_tx = with_crc(response)
                ser.write(last_tx)
                ser.flush()
        if not chunk:
            with box.lock:
                box.update()


def poll_esp(box, url):
    pattern = re.compile(r"([13~])P (?:Running|Switch|Waiting|Confirmed|Delay|Unlocking|Fault|Restarting)")
    while True:
        try:
            with urllib.request.urlopen(url, timeout=3) as resp:
                match = pattern.search(resp.read().decode("utf-8", "replace"))
            phases = {"1": 1, "3": 3, "~": 0}[match.group(1)] if match else None
        except OSError:
            phases = None
        if phases is not None:
            with box.lock:
                box.esp_phases = phases
                box.update()
        time.sleep(1)


def write_grid(box, path):
    """Write the simulated grid power for an evcc custom meter."""
    while True:
        with box.lock:
            box.update()
            grid = box.grid_w()
        with open(path + ".tmp", "w") as f:
            f.write(f"{grid:.0f}\n")
        os.replace(path + ".tmp", path)
        time.sleep(1)


def serve_control(box, port, stop):
    server = socket.create_server(("127.0.0.1", port))
    while True:
        conn, _ = server.accept()
        with conn, conn.makefile("r") as lines:
            for line in lines:
                if not box.command(line):
                    stop.set()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default="/dev/ttyUSB0", help="RS485 adapter")
    parser.add_argument("--esp-url", help="ESP32 start page, e.g. http://192.168.178.51/")
    parser.add_argument("--slave", type=int, default=1)
    parser.add_argument("--boot-delay", type=float, default=5, help="seconds silent after power-on (real box: about 20)")
    parser.add_argument("--unlock-delay", type=float, default=3, help="seconds status 10 after an unlock")
    parser.add_argument("--car-start-delay", type=float, default=2, help="seconds until the car draws current")
    parser.add_argument("--car-max", type=float, default=16, help="maximum car current in A")
    parser.add_argument("--car", action="store_true", help="start with a connected car")
    parser.add_argument("--energy", type=int, default=1000000, help="initial energy counter in Wh")
    parser.add_argument("--control-port", type=int, default=8502)
    parser.add_argument("--verbose", action="store_true", help="log every frame")
    parser.add_argument("--grid-file", help="write the grid power (W) to this file for evcc")
    parser.add_argument("--pv", type=int, default=0, help="initial PV power in W")
    parser.add_argument("--house", type=int, default=500, help="initial house load in W")
    args = parser.parse_args()

    box = Wallbox(args)
    stop = threading.Event()
    threading.Thread(target=serve_rtu, args=(box, args.port), daemon=True).start()
    threading.Thread(target=serve_control, args=(box, args.control_port, stop), daemon=True).start()
    if args.grid_file:
        threading.Thread(target=write_grid, args=(box, args.grid_file), daemon=True).start()
    if args.esp_url:
        threading.Thread(target=poll_esp, args=(box, args.esp_url), daemon=True).start()

    def read_stdin():
        for line in sys.stdin:
            if not box.command(line):
                break
        stop.set()

    if sys.stdin.isatty():
        threading.Thread(target=read_stdin, daemon=True).start()
    log(f"simulating Heidelberg on {args.port}, control port {args.control_port}")
    try:
        stop.wait()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

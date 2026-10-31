#!/usr/bin/env python3
"""Automated tests on the evcc test bench.

Drives the wallbox simulator, the ESP32 bench board and the local evcc
instance, and checks the results. The bench must be running
(bench/start.sh --background). See bench/README.md.

  bench/bench_test.py            run all tests
  bench/bench_test.py 3 6        run tests 3 and 6
  bench/bench_test.py --list     list the tests
  bench/bench_test.py --soak 60  PV curve with many phase switches for 60 minutes
"""
import argparse
import json
import math
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH_DIR = os.environ.get("BENCH_DIR", os.path.join(os.path.dirname(REPO), "evcc_testbench"))

# evcc log lines that may appear on every test
ALLOWED_ALWAYS = [r"Authentication is disabled"]
# expected while the ESP32 restarts: its TCP server is down for a few seconds,
# and after a restart evcc finds the charger disabled (booting at 0 A)
ALLOWED_ON_RESTART = [
    r"i/o timeout", r"dial tcp", r"connection refused", r"connection reset",
    r"broken pipe", r"\bEOF\b", r"exception '11'", r"charger out of sync",
]
# expected while evcc itself starts
ALLOWED_ON_EVCC_START = [r"charger out of sync", r"sponsor", r"version"]
# never acceptable
FORBIDDEN = [r"invalid status", r"exception '4'", r"phases mismatch"]


class TestFailure(Exception):
    pass


class TestSkipped(Exception):
    pass


def known_issue(description):
    """Mark a test that currently fails because of a known, open problem."""
    def mark(test):
        test.known_issue = description
        return test
    return mark


def load_env():
    env = {"RS485": "/dev/ttyUSB0", "EVCC_PORT": "7071", "SIM_PORT": "8502", "POWER_SWITCH": ""}
    path = os.path.join(BENCH_DIR, "bench.env")
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    key, value = line.split("=", 1)
                    env[key.strip()] = value.strip()
    env.update({k: v for k, v in os.environ.items() if k in env or k == "ESP_IP"})
    if "ESP_IP" not in env:
        sys.exit(f"ESP_IP unknown: run bench/start.sh --init or set ESP_IP ({path})")
    return env


class Bench:
    def __init__(self, env, verbose):
        self.esp_ip = env["ESP_IP"]
        self.esp = f"http://{self.esp_ip}"
        self.evcc = f"http://127.0.0.1:{env['EVCC_PORT']}/api"
        self.sim_port = int(env["SIM_PORT"])
        self.evcc_log = os.path.join(BENCH_DIR, "evcc.log")
        self.sim_log = os.path.join(BENCH_DIR, "sim.log")
        self.power_switch = env["POWER_SWITCH"].rstrip("/")
        self.verbose = verbose
        self.log_mark = 0
        self.sim_log_mark = 0

    def note(self, msg):
        print(f"    {time.strftime('%H:%M:%S')} {msg}", flush=True)

    # simulator

    def sim(self, command):
        with socket.create_connection(("127.0.0.1", self.sim_port), timeout=5) as conn:
            conn.sendall((command + "\n").encode())
            reply = conn.makefile("r").readline().strip()
        if reply.startswith("error"):
            raise TestFailure(f"simulator: {command}: {reply}")
        return reply

    def sim_state(self):
        return json.loads(self.sim("json"))

    # ESP32

    def esp_page(self):
        try:
            with urllib.request.urlopen(self.esp + "/", timeout=3) as resp:
                text = re.sub(r"<[^>]*>", " ", resp.read().decode("utf-8", "replace"))
        except OSError:
            return None
        match = re.search(r"([13~])P (\w+)(?: \[(0x[0-9A-F]+)\])?", text)
        return match.groups() if match else None

    def esp_text(self):
        try:
            with urllib.request.urlopen(self.esp + "/", timeout=3) as resp:
                return re.sub(r"<[^>]*>", " ", resp.read().decode("utf-8", "replace"))
        except OSError:
            return None

    def esp_status_page(self):
        """Status page as "name = value" lines."""
        try:
            with urllib.request.urlopen(self.esp + "/status", timeout=3) as resp:
                html = resp.read().decode("utf-8", "replace")
        except OSError:
            return ""
        return re.sub(r"<[^>]*>", "\n", html.replace("</td><td>", " = "))

    def esp_state(self):
        page = self.esp_page()
        return page[1] if page else None

    def esp_fault(self):
        page = self.esp_page()
        return page[2] if page else None

    def esp_reboot(self):
        req = urllib.request.Request(self.esp + "/reboot", method="POST")
        urllib.request.urlopen(req, timeout=5).read()
        self.note("ESP32 reboot requested")
        self.wait_esp_down()

    def wait_esp_down(self):
        self.wait(lambda: self.esp_state() not in ("Running", "Restarting"), 30,
                  "ESP32 going down for the restart")

    def wait_esp_running(self, timeout=120):
        self.wait(lambda: self.esp_state() == "Running", timeout, "ESP32 in Running")

    def esp_write(self, register, value):
        """Write a holding register over the Modbus TCP bridge, like evcc does."""
        pdu = struct.pack(">BHH", 6, register, value)
        frame = struct.pack(">HHHB", 1, 0, len(pdu) + 1, 1) + pdu
        with socket.create_connection((self.esp_ip, 502), timeout=5) as conn:
            conn.sendall(frame)
            reply = conn.recv(256)
        if len(reply) < 9 or reply[7] & 0x80:
            raise TestFailure(f"Modbus write {register}={value} failed: {reply.hex()}")
        self.note(f"Modbus write register {register} = {value}")

    def esp_ota(self, path):
        with open(path, "rb") as f:
            data = f.read()
        req = urllib.request.Request(self.esp + "/update?name=" + os.path.basename(path),
                                     data=data, method="POST",
                                     headers={"Content-Type": "application/octet-stream"})
        reply = urllib.request.urlopen(req, timeout=180).read().decode(errors="replace")
        self.note(f"OTA upload: {reply.strip()}")
        if "successful" not in reply:
            raise TestFailure(f"OTA upload failed: {reply}")
        self.wait_esp_down()

    # board power (Tasmota plug in front of the board's power supply)

    def power_state(self):
        with urllib.request.urlopen(self.power_switch + "/?m=1", timeout=5) as resp:
            text = re.sub(r"<[^>]*>", " ", resp.read().decode("utf-8", "replace"))
        match = re.search(r"\b(ON|OFF)\s*$", text.strip())
        if not match:
            raise TestFailure(f"cannot read the power switch state: {text[-80:]!r}")
        return match.group(1) == "ON"

    def power_set(self, on):
        """Switch the board's power. Uses the Tasmota web UI toggle, which
        works without enabling the command API."""
        if not self.power_switch:
            raise TestSkipped("no POWER_SWITCH in bench.env")
        if self.power_state() != on:
            urllib.request.urlopen(self.power_switch + "/?m=1&o=1", timeout=5).read()
        self.wait(lambda: self.power_state() == on, 10, f"power switch {'on' if on else 'off'}")
        self.note(f"board power {'on' if on else 'off'}")

    def power_cycle(self, off_seconds=15):
        """Cut the board's power. The simulator learns about the wallbox power
        from the board's web page, which is gone while the board is off, so
        the simulated wallbox is switched off explicitly."""
        self.power_set(False)
        self.sim("phases 0")
        try:
            time.sleep(off_seconds)
        finally:
            self.power_set(True)
            self.sim("phases auto")

    # evcc

    def lp(self):
        try:
            with urllib.request.urlopen(self.evcc + "/state", timeout=5) as resp:
                return json.load(resp)["loadpoints"][0]
        except (OSError, ValueError, KeyError, IndexError):
            return None

    def evcc_post(self, path):
        req = urllib.request.Request(f"{self.evcc}/loadpoints/1/{path}", method="POST")
        try:
            urllib.request.urlopen(req, timeout=5).read()
        except urllib.error.HTTPError as err:
            raise TestFailure(f"evcc POST {path}: HTTP {err.code}")
        self.note(f"evcc: {path}")

    def evcc_stop(self):
        pid_file = os.path.join(BENCH_DIR, "evcc.pid")
        with open(pid_file) as f:
            pid = int(f.read())
        os.kill(pid, 15)
        self.wait(lambda: self.lp() is None, 30, "evcc stopped")
        self.note("evcc stopped")

    def evcc_start(self):
        log = open(self.evcc_log, "a")
        proc = subprocess.Popen(
            [os.path.join(BENCH_DIR, "evcc"), "--config", os.path.join(BENCH_DIR, "evcc.yaml"),
             "--database", os.path.join(BENCH_DIR, "evcc.db"),
             "--disable-auth", "--disable-onboarding"],
            stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
            start_new_session=True)
        with open(os.path.join(BENCH_DIR, "evcc.pid"), "w") as f:
            f.write(str(proc.pid))
        # the API answers before the loadpoint and its charger are ready
        self.wait(lambda: self.lp()["chargerPhases1p3p"], 90, "evcc charger initialized")
        self.settle(5)
        self.note("evcc started")

    def charging(self, phases=None):
        """evcc charges, and with phases: the board finished switching and
        current flows on exactly that many phases."""
        lp = self.lp()
        if not lp or not lp.get("charging"):
            return False
        st = self.sim_state()
        # evcc's flag can lag behind (e.g. while the board is unreachable)
        if not any(c > 0 for c in st["currents"]):
            return False
        if phases is None:
            return True
        return (st["powered_phases"] == phases
                and sum(1 for c in st["currents"] if c > 0) == phases
                and self.esp_state() == "Running")

    # logs

    def mark_log(self):
        self.log_mark = os.path.getsize(self.evcc_log)
        self.sim_log_mark = os.path.getsize(self.sim_log)

    def log_since_mark(self):
        with open(self.evcc_log, errors="replace") as f:
            f.seek(self.log_mark)
            return f.read().splitlines()

    def sim_log_since_mark(self):
        with open(self.sim_log, errors="replace") as f:
            f.seek(self.sim_log_mark)
            return [line for line in f.read().splitlines() if " rx " not in line]

    def check_log(self, allowed=(), forbidden=()):
        """Fail on unexpected evcc log lines. Patterns in allowed may also
        lift a FORBIDDEN one, for tests that provoke it on purpose."""
        problems = []
        always_forbidden = [p for p in FORBIDDEN if p not in allowed]
        for line in self.log_since_mark():
            if any(re.search(p, line) for p in always_forbidden + list(forbidden)):
                problems.append(line)
            elif re.search(r"\b(ERROR|WARN|FATAL)\b", line) and not any(
                    re.search(p, line) for p in ALLOWED_ALWAYS + list(allowed)):
                problems.append(line)
        if problems:
            raise TestFailure("unexpected evcc log lines:\n      " + "\n      ".join(problems[:10]))

    # helpers

    def wait(self, condition, timeout, description):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                if condition():
                    if self.verbose:
                        self.note(f"ok: {description}")
                    return
            except (OSError, ValueError, KeyError, TypeError):
                pass
            time.sleep(1)
        raise TestFailure(f"timeout after {timeout} s: {description}")

    def settle(self, seconds):
        time.sleep(seconds)

    def start_charging(self, pv=6000, phases=None):
        self.sim("car on")
        self.sim(f"pv {pv}")
        self.wait(lambda: self.charging(phases), 240,
                  f"evcc charging{f' on {phases}p' if phases else ''}")


# tests ---------------------------------------------------------------------
# Every test starts from prepare(): car unplugged, no PV, PV mode, auto phases.

def test_boot(b):
    """Boot: ESP32 reaches Running, evcc sees a 1p3p charger"""
    b.esp_reboot()
    b.wait_esp_running()
    b.wait(lambda: b.lp() and b.lp()["chargerPhases1p3p"], 60, "evcc charger with phase switching")
    b.settle(20)
    b.check_log(allowed=ALLOWED_ON_RESTART)


def test_pv_charging(b):
    """PV charging: car + 6 kW PV, evcc charges 3p at about 8 A"""
    b.sim("car on")
    b.sim("pv 6000")
    b.wait(lambda: b.lp()["connected"], 30, "evcc: car connected")
    b.wait(lambda: b.charging(3), 240, "evcc charging on 3p")
    b.settle(30)
    st = b.sim_state()
    b.note(f"currents {st['currents']} A, grid {st['grid_w']} W")
    if not all(6.5 <= c <= 10 for c in st["currents"]):
        raise TestFailure(f"expected about 8 A on all phases, got {st['currents']}")
    if abs(st["grid_w"]) > 600:
        raise TestFailure(f"expected grid close to 0 W, got {st['grid_w']} W")
    b.check_log()


def test_auto_1p(b):
    """Automatic 3p -> 1p: 3 kW PV, evcc switches to 1p at about 11 A"""
    b.start_charging(phases=3)
    b.mark_log()
    b.sim("pv 3000")
    b.wait(lambda: b.charging(1), 240, "evcc charging on 1p")
    b.settle(30)
    st = b.sim_state()
    b.note(f"currents {st['currents']} A, grid {st['grid_w']} W")
    if not 9 <= st["currents"][0] <= 13:
        raise TestFailure(f"expected about 11 A on L1, got {st['currents']}")
    b.check_log()


def test_auto_3p(b):
    """Automatic 1p -> 3p: 7 kW PV, evcc switches back to 3p"""
    b.start_charging(pv=3000, phases=1)
    b.mark_log()
    b.sim("pv 7000")
    b.wait(lambda: b.charging(3), 240, "evcc charging on 3p")
    b.check_log()


def test_manual_switch(b):
    """Manual switch via evcc: phases 1 and 3 switch at once"""
    b.start_charging(pv=7000)
    b.mark_log()
    try:
        b.evcc_post("phases/1")
        b.wait(lambda: b.charging(1), 120, "charging on 1p after manual switch")
        b.evcc_post("phases/3")
        b.wait(lambda: b.charging(3), 120, "charging on 3p after manual switch")
    finally:
        b.evcc_post("phases/0")
    b.check_log()


def test_current_after_switch(b):
    """Current after 3p -> 1p: the box keeps evcc's 16 A, not 6 A (fix b1ff5b1)"""
    b.start_charging(pv=5000, phases=3)
    b.mark_log()
    try:
        b.evcc_post("phases/1")
        b.wait(lambda: b.charging(1), 120, "charging on 1p")
        b.settle(40)  # several evcc cycles: a stuck 6 A would stay
        st = b.sim_state()
        b.note(f"max current {st['max_current']} A, currents {st['currents']} A")
        if st["max_current"] < 14:
            raise TestFailure(f"expected about 16 A after the switch, got {st['max_current']} A")
    finally:
        b.evcc_post("phases/0")
    b.check_log()


def test_switch_without_evcc(b):
    """Switch without evcc writing a current: restart at most at 6 A"""
    b.start_charging(pv=8000, phases=3)
    b.wait(lambda: b.sim_state()["max_current"] > 7, 60, "charging above 7 A first")
    b.mark_log()
    b.esp_write(4, 1)  # like a switch from the ESP32 web UI: evcc does not know
    b.wait(lambda: b.sim_state()["powered_phases"] == 1 and b.esp_state() == "Running",
           120, "board on 1p and Running")
    restart = [line for line in b.sim_log_since_mark() if "max current" in line]
    b.note(f"restart current: {restart[:1]}")
    if not restart:
        raise TestFailure("the board wrote no restart current")
    value = float(re.search(r"max current ([\d.]+) A", restart[0]).group(1))
    if value > 6:
        raise TestFailure(f"expected a restart current of at most 6 A, got {value} A")
    b.check_log()


def test_switch_while_disabled(b):
    """Switch while charging is off: the box stays at 0 A until evcc enables"""
    b.start_charging(pv=7000)
    b.mark_log()
    try:
        b.evcc_post("mode/off")
        # evcc's "charging" flag can lag behind; the current is what counts
        b.wait(lambda: b.sim_state()["currents"] == [0, 0, 0], 30, "charging stopped")
        b.esp_write(4, 1)
        b.wait(lambda: b.sim_state()["powered_phases"] == 1 and b.esp_state() == "Running",
               120, "board on 1p and Running")
        b.settle(20)
        if b.sim_state()["max_current"] != 0:
            raise TestFailure(f"expected 0 A while off, got {b.sim_state()['max_current']} A")
    finally:
        b.evcc_post("mode/pv")
    b.wait(lambda: b.charging(), 180, "charging again after mode pv")
    b.check_log()


def test_restart_charging(b):
    """Restart while charging: no evcc errors, energy kept, charging resumes"""
    b.start_charging()
    b.mark_log()
    b.esp_reboot()
    b.wait_esp_running()
    b.settle(15)
    if any("car disconnected" in line for line in b.log_since_mark()):
        raise TestFailure("evcc saw the car disconnect during the restart")
    b.wait(lambda: b.charging(), 180, "evcc charging again")
    b.check_log(allowed=ALLOWED_ON_RESTART)


def test_ota_charging(b):
    """OTA update while charging: same as a restart, energy kept"""
    firmware = os.environ.get("FIRMWARE_BIN", os.path.join(REPO, "build", "heidelberg-1p3p.bin"))
    if not os.path.exists(firmware):
        raise TestFailure(f"firmware image missing: {firmware} (set FIRMWARE_BIN)")
    b.start_charging()
    b.mark_log()
    b.esp_ota(firmware)
    b.wait_esp_running()
    b.wait(lambda: b.charging(), 180, "evcc charging again")
    b.check_log(allowed=ALLOWED_ON_RESTART)


def test_restart_no_car(b):
    """Restart without car: no phantom car connect in evcc"""
    b.sim("car off")
    b.wait(lambda: not b.lp()["connected"], 30, "evcc: car disconnected")
    b.mark_log()
    b.esp_reboot()
    b.wait_esp_running()
    b.settle(30)
    if any("car connected" in line for line in b.log_since_mark()):
        raise TestFailure("phantom car connect in evcc during the restart")
    b.check_log(allowed=ALLOWED_ON_RESTART)


def test_restart_on_3p(b):
    """Restart while charging on 3p: board and evcc agree on the phases"""
    b.start_charging(pv=7000, phases=3)
    b.mark_log()
    b.esp_reboot()
    b.wait_esp_running()
    b.wait(lambda: b.charging(), 240, "evcc charging again")
    # evcc syncs the phases with the charger when the car connects
    b.sim("car off")
    b.wait(lambda: not b.lp()["connected"], 30, "evcc: car disconnected")
    b.sim("car on")
    b.wait(lambda: b.lp()["connected"], 30, "evcc: car connected")
    b.settle(20)
    lp, st = b.lp(), b.sim_state()
    b.note(f"evcc phasesActive {lp['phasesActive']}, board {st['powered_phases']}p")
    b.check_log(allowed=ALLOWED_ON_RESTART)


def test_unplug(b):
    """Unplug while charging: evcc sees the car leave, current drops to 0"""
    b.start_charging()
    b.mark_log()
    b.sim("car off")
    b.wait(lambda: not b.lp()["connected"], 30, "evcc: car disconnected")
    b.wait(lambda: b.sim_state()["currents"] == [0, 0, 0], 15, "no current")
    b.check_log()


def test_unplug_during_restart(b):
    """Unplug during a restart: evcc sees the car leave afterwards"""
    b.start_charging()
    b.mark_log()
    b.esp_reboot()
    b.sim("car off")
    b.wait_esp_running()
    b.wait(lambda: not b.lp()["connected"], 60, "evcc: car disconnected")
    b.check_log(allowed=ALLOWED_ON_RESTART)


def test_plug_during_switch(b):
    """Plug in during a phase switch: evcc sees the car afterwards and charges"""
    b.sim("pv 7000")
    b.mark_log()
    other = 1 if b.sim_state()["powered_phases"] == 3 else 3
    b.esp_write(4, other)
    b.wait(lambda: b.esp_state() not in ("Running", None), 10, "switch started")
    b.sim("car on")
    b.wait_esp_running()
    b.wait(lambda: b.lp()["connected"], 60, "evcc: car connected")
    b.wait(lambda: b.charging(), 180, "evcc charging")
    # The switch happened behind evcc's back: evcc notices on the car connect,
    # warns and takes over the board's phases.
    b.check_log(allowed=[r"phases mismatch"])


def test_double_switch(b):
    """Two switch requests in a row: board and evcc end up consistent"""
    b.start_charging(pv=7000)
    b.mark_log()
    b.esp_write(4, 1)
    time.sleep(1)
    b.esp_write(4, 3)  # kept while the first switch runs, run afterwards
    b.wait_esp_running()
    b.wait(lambda: b.charging(), 240, "evcc charging again")
    b.settle(60)  # evcc syncs the phases it reads back
    if b.esp_fault():
        raise TestFailure(f"safety fault {b.esp_fault()}")
    b.check_log()


def test_house_spike(b):
    """House load spike: evcc lowers the current and raises it again"""
    b.start_charging(pv=7000, phases=3)
    b.settle(30)
    before = b.sim_state()["max_current"]
    b.mark_log()
    b.sim("house 2000")
    b.wait(lambda: b.sim_state()["max_current"] < before - 1, 60, "current lowered")
    b.sim("house 500")
    b.wait(lambda: b.sim_state()["max_current"] >= before - 0.5, 60, "current raised again")
    b.check_log()


def test_mode_off(b):
    """Mode off: evcc stops charging, wallbox max current 0"""
    b.start_charging()
    b.mark_log()
    try:
        b.evcc_post("mode/off")
        b.wait(lambda: not b.lp()["charging"], 30, "evcc stopped charging")
        b.wait(lambda: b.sim_state()["max_current"] == 0, 30, "wallbox max current 0")
    finally:
        b.evcc_post("mode/pv")
    b.check_log()


def test_evcc_restart(b):
    """evcc restart while charging: evcc takes over without errors"""
    b.start_charging()
    b.mark_log()
    b.evcc_stop()
    b.evcc_start()
    b.wait(lambda: b.charging(), 180, "evcc charging again")
    b.check_log(allowed=ALLOWED_ON_EVCC_START)


def test_evcc_down(b):
    """evcc gone while charging: the board applies the failsafe current"""
    b.start_charging()
    b.evcc_stop()
    try:
        # Register 257 is 15 s by default; the board checks with the loop.
        b.wait(lambda: b.sim_state()["currents"] == [0, 0, 0], 30,
               "charging stopped (failsafe current 0 A)")
        if "evcc timeout" not in (b.esp_text() or ""):
            raise TestFailure("the board does not show the evcc timeout")
    finally:
        b.evcc_start()
    b.wait(lambda: b.charging(), 180, "evcc charging again")


def test_wallbox_dies(b):
    """Wallbox stops answering while charging: reported, charging resumes after"""
    b.start_charging()
    b.sim("silent on")
    try:
        # The wallbox's own watchdog stops charging after 15 s without requests.
        b.wait(lambda: b.sim_state()["currents"] == [0, 0, 0], 30,
               "charging stopped by the wallbox watchdog")
        b.wait(lambda: "wallbox not responding" in (b.esp_text() or ""), 90,
               "board reports the wallbox as not responding")
        if b.esp_fault():
            raise TestFailure(f"unexpected safety fault {b.esp_fault()}")
    finally:
        b.sim("silent off")
    b.wait(lambda: "wallbox not responding" not in (b.esp_text() or "x"), 30,
           "board reports the wallbox back")
    history = re.search(r"Wallbox history: = ([^\n]*)", b.esp_status_page())
    b.note(f"status page: {history.group(1) if history else '?'}")
    if not history or "not responding since power-on, last" not in history.group(1):
        raise TestFailure("the status page does not list the outage")
    # silent for at least the 60 s detection, at most until the 90 s wait ran out
    took = re.search(r"ago for ([\d.]+) (s|min)", history.group(1))
    seconds = float(took[1]) * (60 if took[2] == "min" else 1) if took else None
    if seconds is None or not 60 <= seconds <= 180:
        raise TestFailure(f"implausible outage duration: {history.group(1)}")
    b.wait(lambda: b.charging(), 180, "evcc charging again")
    # Before the board gives up (60 s) evcc gets exception 4, afterwards 11.
    b.check_log(allowed=[r"exception '4'", r"exception '11'", r"i/o timeout",
                         r"charger out of sync"])


# The board runs on for a moment on its capacitors after the plug is off,
# while the simulated wallbox stops at once: evcc may see one passthrough
# error (exception 4) before the board goes silent.
ALLOWED_ON_POWER_LOSS = ALLOWED_ON_RESTART + [r"exception '4'"]


def test_power_loss_charging(b):
    """Power loss while charging on 3p: cold start, evcc charges again"""
    if not b.power_switch:
        raise TestSkipped("no POWER_SWITCH in bench.env")
    b.start_charging(pv=7000, phases=3)
    b.mark_log()
    b.power_cycle()
    b.wait_esp_running(150)
    restart = re.search(r"Last restart: = ([^\n]*)", b.esp_status_page())
    b.note(f"last restart: {restart.group(1) if restart else '?'}")
    if not restart or not restart.group(1).startswith("power-on"):
        raise TestFailure("the board does not report a power-on restart")
    b.wait(lambda: b.charging(), 240, "evcc charging again")
    # Without the RTC cache the board boots on 1p while evcc expected 3p:
    # evcc warns once and takes over the phases.
    b.check_log(allowed=ALLOWED_ON_POWER_LOSS + [r"phases mismatch"])


def test_power_loss_no_car(b):
    """Power loss without car: cold start without a phantom car connect"""
    if not b.power_switch:
        raise TestSkipped("no POWER_SWITCH in bench.env")
    b.sim("car off")
    b.mark_log()
    b.power_cycle()
    b.wait_esp_running(150)
    b.settle(30)
    if any("car connected" in line for line in b.log_since_mark()):
        raise TestFailure("phantom car connect in evcc after the power loss")
    b.check_log(allowed=ALLOWED_ON_POWER_LOSS)


def test_disable_after_switch(b):
    """Disable right after a switch: the current stays at 0 A"""
    b.start_charging(pv=5000, phases=3)
    try:
        b.evcc_post("phases/1")
        b.wait(lambda: b.sim_state()["powered_phases"] == 1 and b.esp_state() == "Running",
               120, "board on 1p and Running")
        b.mark_log()
        b.evcc_post("mode/off")
        b.settle(90)  # nothing on the board may raise the current again
        written = [line for line in b.sim_log_since_mark() if "max current" in line]
        b.note(f"currents written after mode off: {written}")
        if any(float(re.search(r"max current ([\d.]+) A", line).group(1)) > 0 for line in written):
            raise TestFailure("the current went up after evcc disabled charging")
    finally:
        b.evcc_post("phases/0")
    b.check_log()


def test_dead_wallbox(b):
    """Dead wallbox: silent wallbox after power-on leads to safety fault 0xE104"""
    b.sim("car off")
    b.sim("silent on")
    try:
        b.esp_reboot()
        b.wait(lambda: b.esp_fault() == "0xE104", 150, "safety fault 0xE104")
    finally:
        b.sim("silent off")
    b.esp_reboot()
    b.wait_esp_running()
    b.wait(lambda: b.lp() and b.lp()["chargerPhases1p3p"], 60, "evcc charger back")


def test_busy_bridge(b):
    """Busy Modbus TCP: many requests while charging, no false evcc timeout"""
    b.start_charging(phases=3)  # on 1p evcc would switch to 3p during the test
    before = b.esp_status_page()
    b.mark_log()
    # Register 4 is answered by the board itself, so the requests come fast and
    # often land while the main loop checks the evcc watchdog.
    end, requests = time.monotonic() + 300, 0
    with socket.create_connection((b.esp_ip, 502), timeout=5) as conn:
        while time.monotonic() < end:
            tid = requests % 0x10000
            conn.sendall(struct.pack(">HHHBBHH", tid, 0, 6, 1, 3, 4, 1))
            reply = conn.recv(256)
            if len(reply) < 9 or reply[7] & 0x80:
                raise TestFailure(f"read of register 4 failed: {reply.hex()}")
            requests += 1
    after = b.esp_status_page()
    b.note(f"{requests} requests in 300 s")
    count = lambda page, pattern: int((re.search(pattern, page) or [0, 0])[1])
    timeouts = [count(p, r"(\d+) timeouts since power-on") for p in (before, after)]
    outages = [count(p, r"(\d+)x not responding") for p in (before, after)]
    if timeouts[1] != timeouts[0]:
        raise TestFailure(f"false evcc timeouts: {timeouts[0]} -> {timeouts[1]}")
    if outages[1] != outages[0]:
        raise TestFailure(f"false wallbox outages: {outages[0]} -> {outages[1]}")
    if any("max current 0.0" in line for line in b.sim_log_since_mark()):
        raise TestFailure("the current dropped to 0 A while charging")
    if not b.charging():
        raise TestFailure("charging stopped")
    b.check_log()


def test_parallel_requests(b):
    """Parallel requests: evcc and the web page switch at once, then switch and restart at once"""
    def web_post(path):
        req = urllib.request.Request(b.esp + path, method="POST")
        urllib.request.urlopen(req, timeout=5).read()

    def at_once(*actions):
        barrier = threading.Barrier(len(actions))
        errors = []
        def run(action):
            barrier.wait()
            try:
                action()
            except Exception as e:  # reported below
                errors.append(e)
        threads = [threading.Thread(target=run, args=(a,)) for a in actions]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        if errors:
            raise TestFailure(f"request failed: {errors[0]}")

    def switch_count():
        match = re.search(r"Phase switches:? = (\d+) since power-on", b.esp_status_page())
        return int(match[1]) if match else None

    start = b.sim_state()["powered_phases"]
    other = 3 if start == 1 else 1
    before = switch_count()
    at_once(lambda: b.esp_write(4, other), lambda: web_post(f"/{other}p"))
    b.wait(lambda: b.sim_state()["powered_phases"] == other and b.esp_state() == "Running",
           120, f"board on {other}p")
    b.settle(10)
    after = switch_count()
    b.note(f"switches {before} -> {after}")
    if after != before + 1:
        raise TestFailure(f"expected exactly one switch, counter {before} -> {after}")

    at_once(lambda: b.esp_write(4, start), lambda: web_post("/reboot"))
    b.wait(lambda: re.search(r"Uptime:? = 00:0[0-2]:", b.esp_status_page()), 60, "ESP32 restarted")
    b.wait_esp_running()
    b.wait(lambda: b.sim_state()["powered_phases"] == start and b.esp_state() == "Running",
           120, f"board back on {start}p")
    # the test switches past evcc, so evcc warns once when its phases differ
    b.check_log(allowed=ALLOWED_ON_RESTART + [r"phases mismatch"])


def test_write_multiple_phases(b):
    """Write Multiple Registers on register 4: invalid values rejected, valid ones switch"""
    def write_multiple(addr, values):
        payload = b"".join(struct.pack(">H", v) for v in values)
        pdu = struct.pack(">BHHB", 16, addr, len(values), len(payload)) + payload
        frame = struct.pack(">HHHB", 1, 0, len(pdu) + 1, 1) + pdu
        with socket.create_connection((b.esp_ip, 502), timeout=5) as conn:
            conn.sendall(frame)
            return conn.recv(256)

    # Invalid value 2 on register 4 must return exception 3 (Illegal Data Value)
    reply = write_multiple(4, [2])
    if len(reply) < 9 or reply[7] != (16 | 0x80) or reply[8] != 3:
        raise TestFailure(f"expected exception 3 for value 2, got {reply.hex()}")

    # Overlapping range across register 4 must return exception 2 (Illegal Data Address)
    reply = write_multiple(4, [1, 0])
    if len(reply) < 9 or reply[7] != (16 | 0x80) or reply[8] != 2:
        raise TestFailure(f"expected exception 2 for a range across register 4, got {reply.hex()}")

    # a valid value switches the phases
    start = b.sim_state()["powered_phases"]
    other = 3 if start == 1 else 1
    reply = write_multiple(4, [other])
    if len(reply) < 12 or reply[7] & 0x80:
        raise TestFailure(f"write to register 4 failed: {reply.hex()}")
    b.wait(lambda: b.sim_state()["powered_phases"] == other and b.esp_state() == "Running",
           120, f"board on {other}p after the write")

    reply = write_multiple(4, [start])
    if len(reply) < 12 or reply[7] & 0x80:
        raise TestFailure(f"write back to {start}p failed: {reply.hex()}")
    b.wait(lambda: b.sim_state()["powered_phases"] == start and b.esp_state() == "Running",
           120, f"board back on {start}p")
    b.check_log(allowed=[r"phases mismatch"])


def test_concurrent_rtu_and_status(b):
    """Concurrent RTU & web status: parallel Modbus TCP, POST /status and connection cap"""
    b.start_charging(phases=3)
    before = b.esp_status_page()
    b.mark_log()

    stop_time = time.monotonic() + 30
    errors = []

    def modbus_worker(worker_id):
        tid = worker_id * 1000
        try:
            with socket.create_connection((b.esp_ip, 502), timeout=5) as conn:
                conn.settimeout(5)
                while time.monotonic() < stop_time:
                    tid = (tid + 1) & 0xFFFF
                    # Alternate between input reg 5 (RTU), holding reg 261 (RTU) and reg 4 (virtual)
                    for fc, reg, cnt in ((4, 5, 4), (3, 261, 1), (3, 4, 1)):
                        conn.sendall(struct.pack(">HHHBBHH", tid, 0, 6, 1, fc, reg, cnt))
                        reply = conn.recv(256)
                        if len(reply) < 9 or reply[7] & 0x80:
                            raise TestFailure(f"worker {worker_id} read ({fc},{reg}) failed: {reply.hex()}")
        except Exception as e:
            errors.append(e)

    def status_worker():
        try:
            while time.monotonic() < stop_time:
                req = urllib.request.Request(b.esp + "/status", method="POST")
                urllib.request.urlopen(req, timeout=5).read()
                if "Memory" not in b.esp_status_page():
                    raise TestFailure("incomplete /status page during concurrent access")
        except Exception as e:
            errors.append(e)

    threads = [threading.Thread(target=modbus_worker, args=(i,)) for i in range(3)]
    threads.append(threading.Thread(target=status_worker))
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise TestFailure(f"concurrent worker failed: {errors[0]}")

    # The client cap (MAX_MODBUS_TCP_CLIENTS = 10) includes evcc's own connection.
    time.sleep(2)  # the worker connections are closed, their tasks end
    conns = []
    try:
        for _ in range(12):
            try:
                s = socket.create_connection((b.esp_ip, 502), timeout=2)
                s.settimeout(1)
                conns.append(s)
            except OSError:
                pass
        # At least one of the 12 connections must be closed by the cap when reading
        alive = 0
        for s in conns:
            try:
                s.sendall(struct.pack(">HHHBBHH", 1, 0, 6, 1, 3, 4, 1))
                if len(s.recv(256)) >= 9:
                    alive += 1
            except OSError:
                pass
        b.note(f"connection cap check: {alive}/12 sockets answered")
        if not 1 <= alive <= 9:
            raise TestFailure(f"expected 1..9 answering sockets besides evcc, got {alive}")
    finally:
        for s in conns:
            s.close()

    b.settle(20)  # evcc keeps polling over its own connection
    after = b.esp_status_page()
    count = lambda page, pattern: int((re.search(pattern, page) or [0, 0])[1])
    if count(after, r"(\d+)x not responding") != count(before, r"(\d+)x not responding"):
        raise TestFailure("false wallbox outage during concurrent access")
    if count(after, r"(\d+) timeouts since power-on") != count(before, r"(\d+) timeouts since power-on"):
        raise TestFailure("false evcc timeout during concurrent access")
    if any("max current 0.0" in line for line in b.sim_log_since_mark()):
        raise TestFailure("the current dropped to 0 A during concurrent access")
    if not b.charging(3):
        raise TestFailure("charging stopped during concurrent access")
    b.check_log()


TESTS = [
    test_boot, test_pv_charging, test_auto_1p, test_auto_3p, test_manual_switch,
    test_current_after_switch, test_switch_without_evcc, test_switch_while_disabled,
    test_restart_charging, test_ota_charging, test_restart_no_car, test_restart_on_3p,
    test_unplug, test_unplug_during_restart, test_plug_during_switch, test_double_switch,
    test_house_spike, test_mode_off, test_evcc_restart, test_evcc_down, test_wallbox_dies,
    test_dead_wallbox, test_power_loss_charging, test_power_loss_no_car,
    test_disable_after_switch, test_busy_bridge, test_parallel_requests,
    test_write_multiple_phases, test_concurrent_rtu_and_status,
]

# tests that end in a safety fault on purpose
EXPECTS_FAULT = {test_dead_wallbox}
# a wallbox that does not answer cannot be locked before the power goes off
# and a power loss of the board cuts the wallbox without locking it first
EXPECTS_ENERGY_LOSS = {test_dead_wallbox, test_power_loss_charging, test_power_loss_no_car}


def prepare(b):
    """Common start state: car unplugged, no PV, evcc in PV mode, short delays."""
    b.sim("silent off")
    b.sim("phases auto")
    b.sim("car off")
    b.sim("pv 0")
    b.sim("house 500")
    if b.lp() is None:
        b.evcc_start()
    if b.esp_fault():
        b.note(f"ESP32 in fault {b.esp_fault()}, rebooting")
        b.esp_reboot()
    b.wait_esp_running()
    b.evcc_post("mode/pv")
    b.evcc_post("phases/0")
    b.evcc_post("enable/delay/30")
    b.evcc_post("disable/delay/30")
    b.wait(lambda: not b.lp()["connected"], 30, "evcc: no car")
    # A switch behind evcc's back (by hand, or a previous run) leaves evcc
    # expecting other phases; evcc would log a phases mismatch on the next
    # car connect. Bring the board to the phases evcc expects.
    expected = b.lp()["phasesActive"]
    if expected in (1, 3) and b.sim_state()["powered_phases"] != expected:
        b.note(f"board on {b.sim_state()['powered_phases']}p, evcc expects {expected}p: switching")
        b.esp_write(4, expected)
        b.wait(lambda: b.sim_state()["powered_phases"] == expected and b.esp_state() == "Running",
               120, f"board on {expected}p")


def check_invariants(b, test, lost_before):
    """Checks after every test: no energy lost, no unexpected safety fault."""
    lost = b.sim_state()["energy_lost_wh"] - lost_before
    if lost > 0 and test not in EXPECTS_ENERGY_LOSS:
        raise TestFailure(f"the wallbox lost {lost:.1f} Wh of its energy counter")
    if test not in EXPECTS_FAULT and b.esp_fault():
        raise TestFailure(f"unexpected safety fault {b.esp_fault()}")


def cleanup(b):
    b.sim("car off")
    b.sim("pv 0")
    b.sim("silent off")
    if b.lp() is None:
        b.evcc_start()
    b.evcc_post("enable/delay/60")
    b.evcc_post("disable/delay/60")


def soak(b, minutes):
    """PV following a sine curve between 1 and 8 kW (10 min period)."""
    b.sim("car on")
    lost_before = b.sim_state()["energy_lost_wh"]
    b.mark_log()
    end = time.monotonic() + minutes * 60
    switches, last_phases = 0, None
    while time.monotonic() < end:
        t = time.monotonic() % 600 / 600
        pv = int(4500 + 3500 * math.sin(2 * math.pi * t))
        b.sim(f"pv {pv}")
        st = b.sim_state()
        if last_phases and st["powered_phases"] and st["powered_phases"] != last_phases:
            switches += 1
            b.note(f"switch to {st['powered_phases']}p (#{switches}), pv {pv} W")
        last_phases = st["powered_phases"] or last_phases
        if b.esp_fault():
            raise TestFailure(f"safety fault {b.esp_fault()}")
        time.sleep(10)
    b.note(f"{switches} phase switches")
    check_invariants(b, None, lost_before)
    b.check_log()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tests", nargs="*", type=int, help="test numbers (default: all)")
    parser.add_argument("--list", action="store_true", help="list the tests")
    parser.add_argument("--verbose", action="store_true", help="log every passed wait")
    parser.add_argument("--soak", type=int, metavar="MINUTES", help="run the soak test instead")
    args = parser.parse_args()

    if args.list:
        for i, test in enumerate(TESTS, 1):
            known = " (known issue)" if hasattr(test, "known_issue") else ""
            print(f"{i:2}  {test.__doc__}{known}")
        return 0

    b = Bench(load_env(), args.verbose)
    try:
        b.sim("json")
    except OSError:
        sys.exit(f"simulator not reachable on port {b.sim_port}: start the bench first, "
                 "bench/start.sh (see bench/README.md)")
    # Starting evcc here would race with bench/start.sh, which starts it once
    # the simulated wallbox answers; tests that stop evcc start it themselves.
    deadline = time.monotonic() + 90
    while b.lp() is None:
        if time.monotonic() > deadline:
            sys.exit("evcc not reachable: start the bench with bench/start.sh and wait "
                     "until it prints \"evcc UI: ...\"")
        time.sleep(2)
    if args.soak:
        prepare(b)
        try:
            soak(b, args.soak)
            print("PASS")
            return 0
        except TestFailure as err:
            print(f"FAIL: {err}")
            return 1
        finally:
            cleanup(b)

    selected = args.tests or list(range(1, len(TESTS) + 1))
    results = []
    started = time.monotonic()
    try:
        print("preparing the bench...")
        prepare(b)
        for number in selected:
            test = TESTS[number - 1]
            known = getattr(test, "known_issue", None)
            print(f"\n[{number}] {test.__doc__}", flush=True)
            t0 = time.monotonic()
            try:
                prepare(b)
                lost_before = b.sim_state()["energy_lost_wh"]
            except TestFailure as err:
                results.append((number, test.__doc__, "FAIL", time.monotonic() - t0, f"setup: {err}"))
                print(f"    FAIL in setup: {err}")
                continue
            try:
                b.mark_log()
                test(b)
                check_invariants(b, test, lost_before)
                result = "FIXED?" if known else "PASS"
                detail = "passes although marked as known issue: " + known if known else ""
            except TestSkipped as err:
                result, detail = "SKIP", str(err)
            except TestFailure as err:
                result, detail = ("KNOWN", known) if known else ("FAIL", str(err))
                if known:
                    print(f"    (failure: {err})")
            results.append((number, test.__doc__, result, time.monotonic() - t0, detail))
            print(f"    {result}{': ' + detail if detail else ''}")
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        try:
            cleanup(b)
        except (OSError, TestFailure) as err:
            print(f"cleanup failed: {err}")

    print(f"\n{'#':>2}  {'result':6} {'time':>6}  test")
    for number, doc, result, duration, _ in results:
        print(f"{number:2}  {result:6} {duration:5.0f}s  {doc}")
    counts = {r: sum(1 for x in results if x[2] == r) for r in ("PASS", "FAIL", "KNOWN", "FIXED?", "SKIP")}
    print(f"\n{counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIP']} skipped, {counts['KNOWN']} known issues, "
          f"{counts['FIXED?']} known issues passing, {(time.monotonic() - started) / 60:.1f} min")
    return 1 if counts["FAIL"] or len(results) < len(selected) else 0


if __name__ == "__main__":
    sys.exit(main())

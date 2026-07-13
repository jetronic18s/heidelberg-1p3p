#!/usr/bin/env python3
import argparse
import random
import socket
import struct
import threading
import time


def build_request(tx_id, unit_id, function_code, register, count):
    pdu = struct.pack(">BHH", function_code, register, count)
    mbap = struct.pack(">HHHB", tx_id, 0, len(pdu) + 1, unit_id)
    return mbap + pdu


def recv_exact(sock, length):
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise ConnectionError("socket closed while receiving")
        data.extend(chunk)
    return bytes(data)


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.normal_ok = 0
        self.normal_fail = 0
        self.invalid_exception_ok = 0
        self.invalid_unexpected_ok = 0
        self.transport_errors = 0
        self.partial_closes = 0
        self.attempts = 0

    def inc(self, field):
        with self.lock:
            setattr(self, field, getattr(self, field) + 1)

    def snapshot(self):
        with self.lock:
            return {
                "normal_ok": self.normal_ok,
                "normal_fail": self.normal_fail,
                "invalid_exception_ok": self.invalid_exception_ok,
                "invalid_unexpected_ok": self.invalid_unexpected_ok,
                "transport_errors": self.transport_errors,
                "partial_closes": self.partial_closes,
                "attempts": self.attempts,
            }


def worker(args, stats, worker_id, deadline):
    rng = random.Random(args.seed + worker_id)
    tx_id = worker_id * 1000
    normal_cases = [
        (0x03, 259, 1),
        (0x03, 261, 1),
        (0x03, 256, 8),
        (0x04, 5, 1),
        (0x04, 10, 3),
    ]
    invalid_case = (0x03, 65000, 1)

    while time.time() < deadline:
        stats.inc("attempts")
        mode_roll = rng.random()
        is_partial_close = mode_roll < args.partial_close_ratio
        is_invalid = args.partial_close_ratio <= mode_roll < (args.partial_close_ratio + args.invalid_ratio)

        try:
            with socket.create_connection((args.host, args.port), timeout=args.timeout_s) as sock:
                sock.settimeout(args.timeout_s)

                if is_partial_close:
                    tx_id = (tx_id + 1) & 0xFFFF
                    raw = build_request(tx_id, args.unit_id, 0x03, 259, 1)
                    sock.sendall(raw[:4])
                    stats.inc("partial_closes")
                    continue

                if is_invalid:
                    function_code, register, count = invalid_case
                else:
                    function_code, register, count = rng.choice(normal_cases)

                tx_id = (tx_id + 1) & 0xFFFF
                req = build_request(tx_id, args.unit_id, function_code, register, count)
                sock.sendall(req)

                mbap = recv_exact(sock, 7)
                rx_tx_id, proto_id, length, unit = struct.unpack(">HHHB", mbap)
                if proto_id != 0 or rx_tx_id != tx_id or unit != args.unit_id or length < 2:
                    raise ValueError("invalid MBAP response")

                pdu = recv_exact(sock, length - 1)
                rx_fc = pdu[0]
                is_exception = (rx_fc & 0x80) != 0

                if is_invalid:
                    if is_exception:
                        stats.inc("invalid_exception_ok")
                    else:
                        stats.inc("invalid_unexpected_ok")
                else:
                    if is_exception:
                        stats.inc("normal_fail")
                    else:
                        stats.inc("normal_ok")

        except Exception:
            stats.inc("transport_errors")
            # Avoid tight busy loops when the host is unreachable.
            time.sleep(max(args.sleep_ms / 1000.0, 0.005))
            continue

        if args.sleep_ms > 0:
            time.sleep(args.sleep_ms / 1000.0)


def main():
    parser = argparse.ArgumentParser(description="Parallel Modbus-TCP stress test for Heidelberg bridge")
    parser.add_argument("--host", default="192.168.178.51")
    parser.add_argument("--port", type=int, default=502)
    parser.add_argument("--unit-id", type=int, default=1)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--duration-s", type=int, default=120)
    parser.add_argument("--timeout-s", type=float, default=1.5)
    parser.add_argument("--sleep-ms", type=int, default=0)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--invalid-ratio", type=float, default=0.20, help="fraction of requests using invalid address")
    parser.add_argument("--partial-close-ratio", type=float, default=0.10, help="fraction of connections closed mid-frame")
    parser.add_argument("--max-normal-fail", type=int, default=0)
    parser.add_argument("--max-transport-errors", type=int, default=0)
    args = parser.parse_args()

    if args.clients < 1:
        raise SystemExit("clients must be >= 1")
    if args.duration_s < 1:
        raise SystemExit("duration-s must be >= 1")
    if args.invalid_ratio < 0 or args.partial_close_ratio < 0 or (args.invalid_ratio + args.partial_close_ratio) > 0.95:
        raise SystemExit("invalid ratio configuration")

    # Fast pre-check to fail early on unreachable host/port.
    try:
        with socket.create_connection((args.host, args.port), timeout=args.timeout_s):
            pass
    except Exception as exc:
        raise SystemExit(f"connectivity check failed for {args.host}:{args.port}: {exc}")

    print(f"[modbus-stress] target={args.host}:{args.port} unit={args.unit_id} clients={args.clients} duration={args.duration_s}s")
    stats = Stats()
    deadline = time.time() + args.duration_s
    threads = []
    for i in range(args.clients):
        t = threading.Thread(target=worker, args=(args, stats, i, deadline), daemon=True)
        threads.append(t)
        t.start()
    for t in threads:
        t.join()

    snap = stats.snapshot()
    print("[modbus-stress] summary")
    for key in ["attempts", "normal_ok", "normal_fail", "invalid_exception_ok", "invalid_unexpected_ok", "transport_errors", "partial_closes"]:
        print(f"  {key}: {snap[key]}")

    failed = False
    if snap["normal_ok"] == 0:
        print("[modbus-stress] FAIL: no successful normal requests")
        failed = True
    if snap["normal_fail"] > args.max_normal_fail:
        print(f"[modbus-stress] FAIL: normal_fail {snap['normal_fail']} > max_normal_fail {args.max_normal_fail}")
        failed = True
    if snap["invalid_unexpected_ok"] > 0:
        print(f"[modbus-stress] FAIL: invalid requests succeeded {snap['invalid_unexpected_ok']} times")
        failed = True
    if snap["transport_errors"] > args.max_transport_errors:
        print(f"[modbus-stress] FAIL: transport_errors {snap['transport_errors']} > max_transport_errors {args.max_transport_errors}")
        failed = True

    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()

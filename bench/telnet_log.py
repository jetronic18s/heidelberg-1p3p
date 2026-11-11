#!/usr/bin/env python3
"""Record the ESP32's telnet debug log with the host's wall-clock time.

Reconnects after every restart of the board. Usage: telnet_log.py <ip> [port]
"""
import socket
import sys
import time


def main():
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 23
    while True:
        try:
            with socket.create_connection((host, port), timeout=5) as sock:
                print(time.strftime("%H:%M:%S"), "--- connected", flush=True)
                sock.settimeout(60)
                pending = b""
                while True:
                    data = sock.recv(4096)
                    if not data:
                        break
                    pending += data
                    *lines, pending = pending.split(b"\n")
                    stamp = time.strftime("%H:%M:%S")
                    for line in lines:
                        text = line.decode(errors="replace").rstrip("\r")
                        if text:
                            print(stamp, text, flush=True)
        except OSError as err:
            print(time.strftime("%H:%M:%S"), f"--- disconnected: {err}", flush=True)
        time.sleep(1)


if __name__ == "__main__":
    main()

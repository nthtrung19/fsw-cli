#!/usr/bin/env python3
"""Fake FSW console: sends example console log lines over UDP, to watch them in
mcs (F1, press F3 for the FSW CONSOLE pane). No FSW logic, just sample text.

    python3 fake_console.py [--host 127.0.0.1] [--port 8898] [--interval 0.5]
"""

import argparse
import random
import socket
import time

STARTUP = [
    "RTEMS Kernel Image With LibBSD Booting",
    "Setting up filesystems.",
    "SD: OK",
    "1980-001-00:00:01.20000 ES Startup: CFE_ES_Main in EARLY_INIT state",
    "EVS Port1 31/1/CFE_EVS 1: cFE EVS Initialized. cFE Version 6.7.0.0",
    "1980-001-00:00:02.50000 ES Startup: Loading file: /cf/main.so, APP: MAIN_APP",
    "EVS Port1 31/1/MAIN_APP 1: MAIN_APP Initialized.",
    "1980-001-00:00:03.10000 ES Startup: CFE_ES_Main entering OPERATIONAL state",
]

PROMPT = "SHLL [/] # "   # the RTEMS shell prompt has no line end


def example_line(start):
    """One random example line."""
    uptime = time.monotonic() - start
    rw = " ".join(f"{random.uniform(-0.05, 0.05):.6f}" for _ in range(4))
    return random.choice([
        "EVS Port1 31/1/AOCS_APP 10: Attitude mode changed to NADIR POINTING",
        "EVS Port1 31/1/PBM 17: PCDU read OK: state of charge 87 %",
        "EVS Port1 31/1/TCS 24: Heater 2 ON, temperature -4.3 C",
        "IO_Lib: -1205 - UartRcvTimeout Error: Timedout",
        "IMU: Checksum mismatch!",
        "NadirPointing_Control",
        f"Current_RW = [{rw}]",
        f"MAIN_NOOP receive NOOP message at {uptime:.6f}",
        f"Sec: {int(uptime)}, SubSec: {random.randint(0, 65535)}",
        "Sent Nadir Pointing Command to AOCS App",
    ])


def main():
    parser = argparse.ArgumentParser(description="Send example FSW console lines over UDP.")
    parser.add_argument("--host", default="127.0.0.1", help="where mcs listens (default 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8898, help="console port (default 8898)")
    parser.add_argument("--interval", type=float, default=0.5, help="seconds between lines (default 0.5)")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.host, args.port)
    start = time.monotonic()
    print(f"sending to {args.host}:{args.port}, Ctrl-C to stop")

    def send(text, end="\r\n"):
        sock.sendto((text + end).encode(), target)
        print(text)

    try:
        for line in STARTUP:
            send(line)
            time.sleep(0.1)
        send(PROMPT, end="")
        while True:
            time.sleep(args.interval)
            send(example_line(start))
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()

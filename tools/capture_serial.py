#!/usr/bin/env python3
"""Capture UART; optionally pulse ESP32 reset via the CH340 RTS line."""
import argparse
import time
from pathlib import Path
import serial

parser = argparse.ArgumentParser()
parser.add_argument("--port", required=True)
parser.add_argument("--seconds", type=float, default=20)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--reset", action="store_true", help="Pulse ESP32 EN via RTS after opening the CH340 port to capture boot")
options = parser.parse_args()
port = serial.Serial(port=None, baudrate=115200, timeout=.25)
port.dtr = False
port.rts = False
port.port = options.port
port.open()
if options.reset:
    port.rts = True
    time.sleep(.1)
    port.rts = False
end = time.monotonic() + options.seconds
with port, options.output.open("wb") as target:
    while time.monotonic() < end:
        data = port.read(port.in_waiting or 1)
        if data:
            target.write(data)
            target.flush()
            print(data.decode("utf-8", errors="replace"), end="", flush=True)

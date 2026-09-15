#!/usr/bin/env python3
"""Read serial output without resetting the ESP32 or logging credentials."""
import argparse
import sys
import time
import serial

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--port', default='/dev/cu.usbserial-0001')
p.add_argument('--seconds', type=float, default=60)
args = p.parse_args()
s = serial.Serial(port=None, baudrate=115200, timeout=1)
s.dtr = False
s.rts = False
s.port = args.port
s.open()
deadline = time.monotonic() + args.seconds
try:
    while time.monotonic() < deadline:
        data = s.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
finally:
    s.close()

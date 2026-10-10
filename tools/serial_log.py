#!/usr/bin/env python3
"""Schreibt das Serial-Log des ESP32-S3 mit Zeitstempel nach stdout und verbindet sich nach einem
Reset/Flash von selbst wieder.

Aufruf: .venv/bin/python -u tools/serial_log.py [/dev/ttyACM0]
"""
import sys
import time
from datetime import datetime

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
while True:
    try:
        with serial.Serial(port, 115200, timeout=1) as ser:
            while True:
                line = ser.readline()
                if line:
                    text = line.decode(errors="replace").rstrip()
                    print(f"{datetime.now():%H:%M:%S.%f}"[:-3] + " " + text, flush=True)
    except (serial.SerialException, OSError):
        time.sleep(0.5)

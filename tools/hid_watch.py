#!/usr/bin/env python3
"""Zeigt, welche Tastendrücke der ESP32-S3 als USB-Tastatur an den Pi liefert (evdev).

Liest alle Eingabegeräte des ESP (USB-VID 303a) und schreibt jeden Tastendruck mit dem
Kernel-Zeitstempel nach stdout. Braucht Leserechte auf /dev/input (Gruppe `input`).

Aufruf: python3 -u tools/hid_watch.py
"""
import os
import re
import select
import struct
import time
from datetime import datetime

KEYS = {14: "Backspace", 28: "Enter", 31: "S", 46: "C", 57: "Space", 103: "Up", 105: "Left", 106: "Right",
        108: "Down", 113: "Mute", 114: "VolumeDown", 115: "VolumeUp", 183: "F13"}
EVENT = struct.Struct("llHHi")
EV_KEY = 1


def open_devices() -> dict:
    devices = {}
    for block in open("/proc/bus/input/devices").read().split("\n\n"):
        if "Vendor=303a" not in block:
            continue
        name = re.search(r'Name="(.*)"', block).group(1)
        event = re.search(r"event\d+", block).group(0)
        try:
            devices[os.open(f"/dev/input/{event}", os.O_RDONLY | os.O_NONBLOCK)] = name
        except OSError:  # gerade eingesteckt: udev hat die Rechte noch nicht gesetzt
            for fd in devices:
                os.close(fd)
            return {}
    return devices


devices = {}
while True:
    if not devices:
        devices = open_devices()
        if not devices:
            time.sleep(0.5)
            continue
        print("lausche auf: " + ", ".join(sorted(set(devices.values()))), flush=True)
    readable, _, _ = select.select(list(devices), [], [], 1)
    for fd in readable:
        try:
            data = os.read(fd, EVENT.size * 64)
        except OSError:  # ESP abgezogen oder neu geflasht
            for old in devices:
                os.close(old)
            devices = {}
            break
        for offset in range(0, len(data), EVENT.size):
            sec, usec, kind, code, value = EVENT.unpack_from(data, offset)
            if kind == EV_KEY and value in (0, 1):
                stamp = datetime.fromtimestamp(sec + usec / 1e6)
                print(f"{stamp:%H:%M:%S.%f}"[:-3] + f" USB {KEYS.get(code, f'code{code}'):<10} {'down' if value else 'up'}", flush=True)

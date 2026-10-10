#!/usr/bin/env python3
"""Wartet ab jetzt auf neue Zeilen im Log von monitor.py und gibt sie zusammengefasst aus.

Für Schritt-für-Schritt-Tests: Taste ansagen, dieses Skript starten, Ergebnis ablesen.

Aufruf: tools/linux/wait_events.py <log> <timeout_s> [button|any|never]
  button: endet, sobald ein 0xFB-Tastendruck samt Loslassen da ist (plus 0.6 s Nachlauf)
  any:    endet 2 s nach dem ersten neuen Ereignis irgendeiner Art
  never:  läuft immer bis zum Timeout (Ruhephase beobachten)
"""
import sys
import time

path, timeout = sys.argv[1], float(sys.argv[2])
mode = sys.argv[3] if len(sys.argv) > 3 else "button"

with open(path) as f:
    f.seek(0, 2)
    deadline = time.monotonic() + timeout
    lines, done_at = [], None
    pressed = released = False
    while time.monotonic() < deadline:
        line = f.readline()
        if not line:
            if done_at and time.monotonic() >= done_at:
                break
            time.sleep(0.05)
            continue
        lines.append(line.rstrip())
        if "id=0xfb" in line:
            if "(losgelassen)" in line:
                released = pressed
            else:
                pressed = True
        if mode == "button" and pressed and released:
            done_at = time.monotonic() + 0.6
        if mode == "any" and done_at is None:
            done_at = time.monotonic() + 2

other = {}
for l in lines:
    if "REPORT" in l and "id=0xfb" not in l:
        rid = l.split("id=")[1].split()[0]
        other[rid] = other.get(rid, 0) + 1
    else:
        print(l)
if other:
    print("weitere Reports:", ", ".join(f"{k} x{v}" for k, v in other.items()))
if not lines:
    print("NICHTS empfangen")

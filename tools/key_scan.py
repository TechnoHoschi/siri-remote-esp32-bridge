#!/usr/bin/env python3
"""Zeigt, welche Tasten ein USB-Eingabegerät sendet – z. B. ein Flirc mit einer Fernbedienung, deren
Belegung am Mediaplayer schon funktioniert. So lässt sich eine bewährte Belegung für die Bridge abschauen.

Liest unter Linux alle Eingabegeräte, deren Name oder USB-Vendor zum Suchbegriff passt, und schreibt jeden
Tastendruck mit Linux-Tastencode und – soweit bekannt – der HID-Usage, die in der Weboberfläche der Bridge
einzutragen wäre. Braucht Leserechte auf /dev/input (Gruppe `input`).

Aufruf: python3 -u tools/key_scan.py [suchbegriff]      Standard: flirc (Vendor 20a0)
"""
import os
import re
import select
import struct
import sys
import time
from datetime import datetime

EVENT = struct.Struct("llHHi")
EV_KEY = 1

# Linux-Tastencode -> (Name, Art, HID-Usage). Art: K = Tastatur (Keyboard-Page), C = Multimedia (Consumer-Page)
KEYS = {
    1: ("Esc", "K", 0x29), 14: ("Backspace", "K", 0x2A), 15: ("Tab", "K", 0x2B), 28: ("Enter", "K", 0x28),
    57: ("Leertaste", "K", 0x2C), 102: ("Pos1", "K", 0x4A), 107: ("Ende", "K", 0x4D), 104: ("Bild auf", "K", 0x4B),
    109: ("Bild ab", "K", 0x4E), 110: ("Einfg", "K", 0x49), 111: ("Entf", "K", 0x4C), 103: ("Pfeil hoch", "K", 0x52),
    108: ("Pfeil runter", "K", 0x51), 105: ("Pfeil links", "K", 0x50), 106: ("Pfeil rechts", "K", 0x4F),
    127: ("Menütaste", "K", 0x65), 96: ("Enter (Ziffernblock)", "K", 0x58), 353: ("OK (Steuerkreuz)", "C", 0x41), 582: ("Sprachassistent", "C", 0xCF), 12: ("-", "K", 0x2D), 13: ("=", "K", 0x2E),
    113: ("Stumm", "C", 0xE2), 114: ("Leiser", "C", 0xEA), 115: ("Lauter", "C", 0xE9), 164: ("Play/Pause", "C", 0xCD),
    207: ("Play", "C", 0xB0), 119: ("Pause", "C", 0xB1), 166: ("Stop", "C", 0xB7), 163: ("Nächster Titel", "C", 0xB5),
    165: ("Vorheriger Titel", "C", 0xB6), 208: ("Vorspulen", "C", 0xB3), 168: ("Zurückspulen", "C", 0xB4),
    172: ("Home", "C", 0x223), 158: ("Zurück", "C", 0x224), 217: ("Suche", "C", 0x221), 139: ("Menü", "C", 0x40),
    402: ("Kanal +", "C", 0x9C), 403: ("Kanal −", "C", 0x9D), 116: ("Power", "C", 0x30), 142: ("Standby", "C", 0x32),
    29: ("Strg links", "K", 0xE0), 42: ("Umschalt links", "K", 0xE1), 56: ("Alt links", "K", 0xE2), 125: ("Super links", "K", 0xE3),
}
LETTERS = {16: "Q", 17: "W", 18: "E", 19: "R", 20: "T", 21: "Y", 22: "U", 23: "I", 24: "O", 25: "P", 30: "A", 31: "S",
           32: "D", 33: "F", 34: "G", 35: "H", 36: "J", 37: "K", 38: "L", 44: "Z", 45: "X", 46: "C", 47: "V", 48: "B",
           49: "N", 50: "M"}
for code, letter in LETTERS.items():
    KEYS[code] = (letter, "K", 0x04 + ord(letter) - ord("A"))
for i in range(9):
    KEYS[2 + i] = (str(i + 1), "K", 0x1E + i)
KEYS[11] = ("0", "K", 0x27)
for i in range(10):
    KEYS[59 + i] = (f"F{i + 1}", "K", 0x3A + i)
KEYS[87], KEYS[88] = ("F11", "K", 0x44), ("F12", "K", 0x45)


def find_devices(needle: str) -> dict:
    devices = {}
    for block in open("/proc/bus/input/devices").read().split("\n\n"):
        name = re.search(r'Name="(.*)"', block)
        event = re.search(r"event\d+", block)
        if not name or not event:
            continue
        if needle not in name.group(1).lower() and f"vendor={needle}" not in block.lower():
            continue
        try:
            devices[os.open(f"/dev/input/{event.group(0)}", os.O_RDONLY | os.O_NONBLOCK)] = name.group(1)
        except OSError:  # gerade eingesteckt: udev hat die Rechte noch nicht gesetzt
            for fd in devices:
                os.close(fd)
            return {}
    return devices


def main() -> None:
    needle = (sys.argv[1] if len(sys.argv) > 1 else "flirc").lower()
    vendor = "20a0" if needle == "flirc" else needle
    devices, announced = {}, False
    while True:
        if not devices:
            devices = find_devices(needle) or find_devices(vendor)
            if not devices:
                if not announced:
                    print(f"warte auf ein Eingabegerät mit „{needle}“ im Namen …", flush=True)
                    announced = True
                time.sleep(0.5)
                continue
            print("lausche auf: " + " | ".join(sorted(set(devices.values()))), flush=True)
        readable, _, _ = select.select(list(devices), [], [], 1)
        for fd in readable:
            try:
                data = os.read(fd, EVENT.size * 64)
            except OSError:  # Gerät abgezogen
                for old in devices:
                    os.close(old)
                devices, announced = {}, False
                break
            for offset in range(0, len(data), EVENT.size):
                sec, usec, kind, code, value = EVENT.unpack_from(data, offset)
                if kind != EV_KEY or value not in (0, 1):
                    continue
                name, page, usage = KEYS.get(code, (f"unbekannt", "?", 0))
                hid = f"{'Tastatur' if page == 'K' else 'Multimedia'} 0x{usage:02X}" if page != "?" else "HID-Usage unbekannt"
                stamp = datetime.fromtimestamp(sec + usec / 1e6)
                print(f"{stamp:%H:%M:%S.%f}"[:-3] + f" {'DOWN' if value else 'UP  '} Linux-Code {code:<3} {name:<16} -> {hid}   [{devices[fd]}]", flush=True)


if __name__ == "__main__":
    main()

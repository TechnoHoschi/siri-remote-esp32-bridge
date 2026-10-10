#!/usr/bin/env python3
"""Dauer-Monitor für die Siri Remote.

Verbindet sich mit der gebondeten Remote, abonniert alle HID-Input-Reports und meldet jedes Ereignis
als dict an einen Callback. Bei Verbindungsverlust wird neu verbunden. Als Skript gestartet schreibt
er jedes Ereignis als Zeile nach stdout; tools/linux/live.py nutzt dieselbe Schleife für die Browser-Ansicht.

Aufruf: .venv/bin/python -u tools/linux/monitor.py <identity-adresse> [--enable none|f000]
"""
import argparse
import asyncio
import time
from datetime import datetime

from bleak import BleakClient, BleakScanner
from bleak.backends.bluezdbus.manager import get_global_bluez_manager
from bleak.backends.device import BLEDevice

HID_SERVICE = "00001812-0000-1000-8000-00805f9b34fb"
REPORT = "00002a4d-0000-1000-8000-00805f9b34fb"
REPORT_REF = "00002908-0000-1000-8000-00805f9b34fb"
REPORT_TYPES = {1: "input", 2: "output", 3: "feature"}
APPLE = 0x004C
BUTTON_REPORT = 0xFB

BUTTONS = {
    0x0001: "Home/TV", 0x0002: "Vol+", 0x0004: "Vol-", 0x0008: "Click", 0x0010: "Power",
    0x0020: "Siri", 0x0040: "Back", 0x0080: "Mute", 0x0100: "Play/Pause", 0x0200: "Up",
    0x0400: "Right", 0x0800: "Down", 0x1000: "Left",
}


def button_names(mask: int) -> list[str]:
    names = [name for bit, name in BUTTONS.items() if mask & bit]
    unknown = mask & ~sum(BUTTONS)
    if unknown:
        names.append(f"?0x{unknown:04x}")
    return names


def format_event(ev: dict) -> str:
    """Ereignis als Log-Zeile (Format wird von wait_events.py und den Fixtures erwartet)."""
    kind = ev["type"]
    if kind == "report":
        text = f"REPORT id=0x{ev['id']:02x} len={len(ev['data']) // 2:2d} {ev['data']}"
        if ev["id"] == BUTTON_REPORT:
            text += f"  0x{ev['mask']:04x} " + ("+".join(ev["names"]) or "(losgelassen)")
    elif kind == "start":
        text = f"START enable={ev['enable']}"
    elif kind == "adv":
        text = f"ADV rssi={ev['rssi']}"
    elif kind == "subscribed":
        text = "SUBSCRIBED " + " ".join(f"0x{rid:02x}:{REPORT_TYPES.get(t, t)}" for rid, t in ev["reports"])
    elif kind == "enable":
        text = f"ENABLE {ev['payload']} -> feature 0xf0 {ev['result']}"
    elif kind == "wait":
        text = f"WAIT ({ev['error']})"
    else:
        text = kind.upper()
    return f"{datetime.fromtimestamp(ev['t']):%H:%M:%S.%f}"[:-3] + " " + text


async def find_device(address: str) -> BLEDevice:
    # Der Objektpfad trägt nach dem Pairing bis zum bluetoothd-Neustart noch die zufällige Adresse,
    # deshalb über die Address-Property (Identity-Adresse) suchen statt den Pfad zu bauen.
    manager = await get_global_bluez_manager()
    for path, ifaces in manager._properties.items():
        if ifaces.get("org.bluez.Device1", {}).get("Address") == address.upper():
            return BLEDevice(address, None, {"path": path, "props": {}})
    raise SystemExit(f"{address} ist BlueZ nicht bekannt (nicht gekoppelt?)")


async def session(device: BLEDevice, enable: str, emit, report_refs: dict) -> None:
    disconnected = asyncio.Event()
    identity = bytes.fromhex(device.address.replace(":", ""))
    seen = False

    def on_adv(_, adv) -> None:
        # Die Remote trägt ihre Identity-Adresse im Apple-Manufacturer-Data; so ist ihr Aufwachen
        # erkennbar, auch wenn die BLE-Adresse rotiert.
        nonlocal seen
        if not seen and identity in adv.manufacturer_data.get(APPLE, b""):
            seen = True
            emit({"type": "adv", "rssi": adv.rssi})

    # Der BCM4345C0 kann kein LL Privacy: ohne laufende Discovery filtert der Kernel auf die
    # Identity-Adresse und sieht die Remote (rotierende Adresse) nie. Also bis zum Connect scannen
    # und danach sofort stoppen – ein Dauer-Scan lässt die Verbindung in den Supervision-Timeout laufen.
    scanner = BleakScanner(detection_callback=on_adv)
    await scanner.start()
    try:
        client = BleakClient(device, timeout=45, disconnected_callback=lambda _: disconnected.set())
        await client.connect()
    finally:
        await scanner.stop()
    try:
        emit({"type": "connected"})
        chars = [c for c in client.services.get_service(HID_SERVICE).characteristics if c.uuid == REPORT]

        def on_report(rid: int):
            def handler(_, data: bytearray) -> None:
                ev = {"type": "report", "id": rid, "data": data.hex()}
                if rid == BUTTON_REPORT:
                    ev["mask"] = int.from_bytes(data[:2], "little")
                    ev["names"] = button_names(ev["mask"])
                emit(ev)
            return handler

        # Report-References ändern sich nicht: nach dem ersten Lesen aus dem Cache nehmen und den
        # Button-Report zuerst abonnieren, damit nach einem Reconnect möglichst wenig Zeit vergeht.
        for char in chars:
            if char.handle not in report_refs:
                ref = next(d for d in char.descriptors if d.uuid == REPORT_REF)
                report_refs[char.handle] = tuple(await client.read_gatt_descriptor(ref.handle))
        chars.sort(key=lambda c: report_refs[c.handle][0] != BUTTON_REPORT)
        for char in chars:
            if "notify" in char.properties:
                await client.start_notify(char, on_report(report_refs[char.handle][0]))
        emit({"type": "subscribed", "reports": [list(report_refs[c.handle]) for c in chars]})

        if enable == "f000":
            # Yanndroid gen-3: F0 00 auf Handle 0x4D = Feature-Report 0xF0
            for char in chars:
                if report_refs[char.handle] == (0xF0, 3):
                    try:
                        await client.write_gatt_char(char, b"\xf0\x00", response=True)
                        emit({"type": "enable", "payload": "f000", "result": "ok"})
                    except Exception as e:
                        emit({"type": "enable", "payload": "f000", "result": f"FEHLER {e}"})

        await disconnected.wait()
    finally:
        emit({"type": "disconnected"})
        try:
            await client.disconnect()
        except Exception:
            pass


async def run(address: str, enable: str, on_event) -> None:
    """Endlosschleife: verbinden, Reports melden, bei Verbindungsverlust neu verbinden."""
    def emit(ev: dict) -> None:
        ev["t"] = time.time()
        on_event(ev)

    device = await find_device(address)
    report_refs: dict = {}
    emit({"type": "start", "enable": enable})
    while True:
        try:
            await session(device, enable, emit, report_refs)
        except Exception as e:  # Remote schläft / Connect-Timeout: einfach weiter versuchen
            emit({"type": "wait", "error": f"{type(e).__name__}: {e}"})
            await asyncio.sleep(0.5)


def parse_args(description: str | None = None) -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=description)
    ap.add_argument("address")
    ap.add_argument("--enable", choices=["none", "f000"], default="none")
    return ap


if __name__ == "__main__":
    args = parse_args().parse_args()
    asyncio.run(run(args.address, args.enable, lambda ev: print(format_event(ev), flush=True)))

#!/usr/bin/env python3
"""Entwicklungsserver für die Weboberfläche des Dongles – ohne den ESP neu zu flashen.

Liefert firmware/siri_bridge/web/index.html aus und lädt die Seite im Browser neu, sobald die Datei
gespeichert wird. Die /api-Aufrufe beantwortet er entweder mit Beispieldaten (Standard) oder reicht sie
an einen echten Dongle im Setup-Modus weiter.

Aufruf: python3 tools/ui_dev.py [--port 8766] [--esp http://<ip-des-dongles>]

Im Beispieldaten-Modus blendet er oben eine Leiste ein, mit der sich die Zustände des Dongles
durchschalten lassen (verbunden, wartet auf Tastendruck, nicht gekoppelt, Hotspot).
"""
import argparse
import json
import pathlib
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

INDEX = pathlib.Path(__file__).resolve().parent.parent / "firmware/siri_bridge/web/index.html"
BUTTONS = ["Home/TV", "Vol+", "Vol-", "Click", "Power", "Siri", "Back", "Mute", "Play/Pause", "Up", "Right", "Down", "Left"]
DEFAULT_MAP = [(1, 0x06), (2, 0xE9), (2, 0xEA), (1, 0x28), (1, 0x16), (0, 0), (1, 0x2A), (2, 0xE2), (1, 0x2C),
               (1, 0x52), (1, 0x4F), (1, 0x51), (1, 0x50)]
SCENARIOS = {"connected": "verbunden", "searching": "wartet auf Tastendruck", "pairing": "nicht gekoppelt", "ap": "Hotspot"}
NETWORKS = [("Heimnetz-IoT", -48, False), ("Heimnetz", -52, False), ("FRITZ!Box 7590 KL", -63, False),
            ("Vodafone-A1B2", -71, False), ("Gastzugang", -74, True), ("WLAN-482913", -80, False), ("o2-WLAN67", -86, False)]


def default_config() -> dict:
    return {"map": [{"n": n, "t": t, "m": 0, "c": c} for n, (t, c) in zip(BUTTONS, DEFAULT_MAP)],
            "ledType": 1, "ledPin": 48, "ledInv": 0, "ledBri": 24, "ledDark": 0, "itv": 12, "stale": 0, "mins": 0, "siri": 1}


class Mock:
    """Beispieldaten im selben Format wie die Firmware (handleState/configJson in siri_bridge.ino)."""

    def __init__(self) -> None:
        self.started = time.time()
        self.scenario = "connected"
        self.cfg = default_config()
        self.ssid = "Heimnetz-IoT"

    def state(self) -> dict:
        ap = self.scenario == "ap"
        s = {"version": "v0.2.1-dev", "uptime": int(time.time() - self.started), "heap": 121840,
             "ble": "connected" if ap else self.scenario,
             "bond": "" if self.scenario == "pairing" else "f0:0d:ca:fe:12:34",
             "wifiMode": "ap" if ap else "sta", "ssid": "siri-bridge-setup" if ap else self.ssid,
             "ip": "192.168.4.1" if ap else "192.168.178.42", "savedSsid": "" if ap else self.ssid,
             "setupLeft": 598, "cfg": self.cfg}
        if s["ble"] == "connected":
            s.update(interval=self.cfg["itv"] * 1.25, rssi=-47, lastKey="Play/Pause", lastKeyAge=int(time.time()) % 90,
                     batt=56, power=0xAF)
        return s

    def live(self) -> dict:
        """Simuliert Tastendrücke: alle 2,5 s wird reihum eine Taste für 0,5 s gedrückt."""
        if self.scenario not in ("connected", "ap"):
            return {"mask": 0, "seq": 0, "last": -1}
        t = time.time() - self.started
        seq = int(t // 2.5)
        order = [8, 9, 10, 11, 12, 3, 6, 0, 1, 2, 7, 4, 5]
        key = order[seq % len(order)]
        return {"mask": (1 << key) if t % 2.5 < 0.5 else 0, "seq": seq + 1, "last": key}

    def post(self, path: str, form: dict) -> tuple[int, str]:
        if path == "/api/config":
            for i, entry in enumerate(self.cfg["map"]):
                if f"m{i}" in form:
                    entry["t"], entry["m"], entry["c"] = (int(x) for x in form[f"m{i}"].split(","))
            for web, key in (("ledType", "ledType"), ("ledPin", "ledPin"), ("ledBri", "ledBri"), ("ledInv", "ledInv"),
                             ("ledDark", "ledDark"), ("itv", "itv"), ("stale", "stale"), ("mins", "mins"), ("siri", "siri")):
                if web in form:
                    self.cfg[key] = int(form[web] or 0)
        elif path == "/api/defaults":
            self.cfg = default_config()
        elif path == "/api/wifi":
            self.ssid = form.get("ssid", "")
            self.scenario = "connected" if self.ssid else "ap"
        elif path == "/api/unpair":
            self.scenario = "pairing"
        elif path == "/api/ota":
            time.sleep(3)
        elif path == "/api/factory-reset":
            self.cfg = default_config()
            self.ssid = ""
            self.scenario = "pairing"
        elif path not in ("/api/setup-off", "/api/reboot"):
            return 404, "nicht gefunden"
        return 200, "ok"


DEV_BAR = """
<div id="devbar" style="display:flex;flex-wrap:wrap;gap:6px;align-items:center;
  padding:6px 12px;background:#111;color:#ddd;font:12px system-ui,sans-serif;border-bottom:1px solid #444">
  <b>ui_dev</b><span>%(mode)s</span>%(buttons)s</div>
<script>
(() => {
  let seen = null;
  setInterval(async () => {
    try { const m = await (await fetch('/dev/mtime')).text(); if (seen && m !== seen) location.reload(); seen = m; } catch (e) {}
  }, 700);
  document.querySelectorAll('#devbar button').forEach(b => b.onclick = async () => { await fetch('/dev/scenario?name=' + b.dataset.s); location.reload(); });
})();
</script>
"""


class Handler(BaseHTTPRequestHandler):
    mock: Mock
    esp: str | None

    def log_message(self, fmt, *args) -> None:
        if not any(quiet in args[0] for quiet in ("/dev/mtime", "/api/state", "/api/live")):
            super().log_message(fmt, *args)

    def reply(self, code: int, body: bytes | str, ctype: str = "text/plain; charset=utf-8") -> None:
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def proxy(self, body: bytes | None) -> None:
        req = urllib.request.Request(self.esp + self.path, data=body, method=self.command)
        if body is not None:
            req.add_header("Content-Type", self.headers.get("Content-Type", "application/x-www-form-urlencoded"))
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                self.reply(r.status, r.read(), r.headers.get("Content-Type", "text/plain"))
        except urllib.error.HTTPError as e:
            self.reply(e.code, e.read())
        except OSError as e:
            self.reply(502, f"Dongle nicht erreichbar: {e}")

    def do_GET(self) -> None:
        url = urllib.parse.urlparse(self.path)
        if url.path == "/":
            html = INDEX.read_text(encoding="utf-8")
            if self.esp:
                mode, buttons = f"echte Daten von {self.esp}", ""
            else:
                mode = "Beispieldaten · Zustand:"
                buttons = "".join(
                    f'<button data-s="{key}" style="font:inherit;padding:2px 8px;border-radius:4px;border:1px solid #555;cursor:pointer;'
                    f'background:{"#2b5fb3" if key == self.mock.scenario else "#222"};color:#eee">{label}</button>'
                    for key, label in SCENARIOS.items())
            bar = DEV_BAR % {"mode": mode, "buttons": buttons}
            # Leiste oben als eigener Block, damit sie nichts von der Seite verdeckt
            self.reply(200, html.replace("<body>", "<body>" + bar, 1), "text/html; charset=utf-8")
        elif url.path == "/dev/mtime":
            self.reply(200, str(INDEX.stat().st_mtime_ns))
        elif url.path == "/dev/scenario":
            name = urllib.parse.parse_qs(url.query).get("name", [""])[0]
            if name in SCENARIOS:
                self.mock.scenario = name
            self.reply(200, self.mock.scenario)
        elif self.esp and url.path.startswith("/api/"):
            self.proxy(None)
        elif url.path == "/api/state":
            self.reply(200, json.dumps(self.mock.state()), "application/json")
        elif url.path == "/api/live":
            self.reply(200, json.dumps(self.mock.live()), "application/json")
        elif url.path == "/api/scan":
            time.sleep(1.5)
            self.reply(200, json.dumps([{"ssid": s, "rssi": r, "open": o} for s, r, o in NETWORKS]), "application/json")
        else:
            self.reply(404, "nicht gefunden")

    def do_POST(self) -> None:
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.esp:
            self.proxy(body)
            return
        path = urllib.parse.urlparse(self.path).path
        form = {} if path == "/api/ota" else {k: v[0] for k, v in urllib.parse.parse_qs(body.decode(), keep_blank_values=True).items()}
        self.reply(*self.mock.post(path, form))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8766)
    ap.add_argument("--esp", help="Adresse eines Dongles im Setup-Modus, z. B. http://192.168.178.42")
    args = ap.parse_args()
    Handler.mock = Mock()
    Handler.esp = args.esp.rstrip("/") if args.esp else None
    print(f"ui_dev: http://0.0.0.0:{args.port}  ({'Proxy zu ' + Handler.esp if Handler.esp else 'Beispieldaten'})", flush=True)
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()

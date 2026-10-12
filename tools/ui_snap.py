#!/usr/bin/env python3
"""Macht Bilder der Weboberfläche ohne Bildschirm – zum Kontrollieren und für die Screenshots in docs/.

Die Seite fragt den Dongle laufend ab; ein Browser ohne Bildschirm wartet deshalb ewig auf „fertig geladen“.
Das Skript friert sie ein: Es setzt die Seite wie build.sh zusammen, holt die Beispieldaten einmal vom
laufenden Entwicklungsserver (tools/ui_dev.py) und lässt Chromium ein Bild davon machen.

Aufruf: python3 tools/ui_snap.py <sprache> <reiter> <breite> <höhe> <ziel.png> [dark]
        reiter: start | tasten | einst        Beispiel: ui_snap.py en tasten 1300 1000 /tmp/tasten.png
Braucht: chromium, einen laufenden ui_dev.py (Port 8766). Für das Auge-Symbol eine Emoji-Schrift.
"""
import pathlib
import subprocess
import sys
import tempfile
import urllib.request

import web_bundle

INDEX = pathlib.Path(__file__).resolve().parent.parent / "firmware/siri_bridge/web/index.html"
DEV = "http://localhost:8766"


def main() -> None:
    lang, tab, width, height, out = sys.argv[1:6]
    theme = "dark" if len(sys.argv) > 6 else "light"
    get = lambda path: urllib.request.urlopen(DEV + path).read().decode()
    stub = f"""<script>
try {{ localStorage.setItem('lang', '{lang}'); localStorage.setItem('theme', '{theme}'); }} catch (e) {{}}
document.documentElement.dataset.theme = '{theme}';
const __data = {{'/api/state': {get('/api/state')}, '/api/builtin': {get('/api/builtin')}, '/api/live': {{mask:0, seq:1, last:-1}}, '/api/scan': []}};
window.fetch = async url => ({{ok: true, json: async () => __data[String(url).split('?')[0]] || {{}}, text: async () => ''}});
window.setInterval = () => 0;
</script>"""
    with tempfile.TemporaryDirectory() as tmp:
        page = pathlib.Path(tmp) / "page.html"
        page.write_text(web_bundle.bundle(INDEX).replace("<body>", "<body>" + stub, 1), encoding="utf-8")
        subprocess.run(["chromium", "--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
                        f"--user-data-dir={tmp}/profile", f"--window-size={width},{height}",
                        f"--screenshot={pathlib.Path(out).resolve()}", f"file://{page}#{tab}"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=90, check=True)
    print(out)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Setzt die Weboberfläche zusammen: web/index.html plus die Sprachdateien web/lang/*.json.

Die Seite enthält die Marke /*LANGS*/{}; an ihrer Stelle landen alle Sprachen als ein Objekt
{"de": {...}, "en": {...}}. Der Dongle liefert so weiterhin eine einzige Datei aus.

Aufruf:  python3 tools/web_bundle.py <index.html> [web_ui.h]
  ohne zweite Angabe: nur prüfen, ob die Sprachdateien zueinander passen (Rückgabewert 1 bei Abweichungen)
  mit web_ui.h:       zusätzlich den Header für die Firmware schreiben
"""
import json
import pathlib
import re
import sys

MARK = "/*LANGS*/{}"
REFERENCE = "en"  # Rückfallsprache der Seite: Hier muss jeder Schlüssel stehen.


def load_langs(index: pathlib.Path) -> dict:
    return {f.stem: json.loads(f.read_text(encoding="utf-8")) for f in sorted((index.parent / "lang").glob("*.json"))}


def sources(index: pathlib.Path) -> list:
    return [index, *sorted((index.parent / "lang").glob("*.json"))]


def bundle(index: pathlib.Path) -> str:
    html = index.read_text(encoding="utf-8")
    assert MARK in html, f"{index.name} enthält die Marke {MARK} nicht"
    data = json.dumps(load_langs(index), ensure_ascii=False, separators=(",", ":"))
    return html.replace(MARK, data.replace("</", "<\\/"))


def check(index: pathlib.Path) -> list:
    """Liefert Meldungen zu fehlenden, überzähligen und in der Seite unbekannten Schlüsseln."""
    langs, problems = load_langs(index), []
    if REFERENCE not in langs:
        return [f"lang/{REFERENCE}.json fehlt"]
    ref = set(langs[REFERENCE])
    for code, texts in langs.items():
        for key in sorted(ref - set(texts)):
            problems.append(f"{code}: fehlt {key}")
        for key in sorted(set(texts) - ref):
            problems.append(f"{code}: überzählig {key}")
        for key, text in texts.items():
            if set(re.findall(r"\{(\w+)\}", text)) != set(re.findall(r"\{(\w+)\}", langs[REFERENCE].get(key, text))):
                problems.append(f"{code}: andere Platzhalter in {key}")
    html = index.read_text(encoding="utf-8")
    used = set(re.findall(r'data-i18n(?:-html|-title)?="([^"]+)"', html)) | set(re.findall(r"""\bt\('([^']+)'""", html))
    used |= {k[1:] for k in re.findall(r"'(@[\w.]+)'", html)}
    for key in sorted(used - ref):
        if not key.endswith("."):  # zusammengesetzte Schlüssel wie 'btn.' + name
            problems.append(f"Seite verwendet unbekannten Schlüssel {key}")
    return problems


def main() -> int:
    index = pathlib.Path(sys.argv[1])
    problems = check(index)
    for line in problems:
        print("Sprachdateien:", line, file=sys.stderr)
    if len(sys.argv) > 2:
        html = bundle(index)
        assert ')HTML"' not in html, "Die Seite enthält das Endezeichen des Raw-Strings"
        pathlib.Path(sys.argv[2]).write_text(
            "// ERZEUGT von firmware/build.sh aus web/index.html und web/lang/*.json – nicht von Hand ändern.\n"
            "#pragma once\n\n#include <Arduino.h>\n\n"
            'static const char INDEX_HTML[] PROGMEM = R"HTML(' + html + ')HTML";\n', encoding="utf-8")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

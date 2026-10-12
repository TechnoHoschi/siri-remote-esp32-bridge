#!/bin/bash
# Baut einen Sketch für den ESP32-S3 und legt die Images unter firmware/dist/ ab:
#   <sketch>-<version>-merged.bin  Komplett-Image (Bootloader + Partitionen + App), flashen an 0x0
#   <sketch>-<version>-ota.bin     nur die App, für OTA-Updates
#
# Aufruf: firmware/build.sh <sketch> [upload]
#   upload: flasht anschließend über den am Pi angesteckten ESP
set -euo pipefail

SKETCH=${1:?Aufruf: firmware/build.sh <sketch> [upload]}
DIR=$(cd "$(dirname "$0")" && pwd)

# 4 MB Embedded Flash, 2 MB Embedded PSRAM (quad), nativer USB-Port als TinyUSB-Gerät (HID + CDC),
# zwei App-Slots für OTA.
FQBN="esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=4M,PartitionScheme=min_spiffs,PSRAM=enabled"

VERSION=$(git -C "$DIR" describe --tags --always --dirty 2>/dev/null || echo dev)
BUILD="$DIR/build/$SKETCH"
DIST="$DIR/dist"
mkdir -p "$BUILD" "$DIST"

# Weboberfläche: web/index.html und web/lang/*.json sind die Quelle, daraus entsteht der Header mit der Seite
# als String. Passen die Sprachdateien nicht zueinander, bricht der Bau ab.
if [ -f "$DIR/$SKETCH/web/index.html" ]; then
  python3 "$DIR/../tools/web_bundle.py" "$DIR/$SKETCH/web/index.html" "$DIR/$SKETCH/web_ui.h"
fi

arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" \
  --build-property "compiler.cpp.extra_flags=-DFW_VERSION=\"$VERSION\" ${EXTRA_FLAGS:-}" "$DIR/$SKETCH"

cp "$BUILD/$SKETCH.ino.merged.bin" "$DIST/$SKETCH-$VERSION-merged.bin"
cp "$BUILD/$SKETCH.ino.bin" "$DIST/$SKETCH-$VERSION-ota.bin"
ls -l "$DIST/$SKETCH-$VERSION-"*.bin

if [ "${2:-}" = upload ]; then
  # Ein mitlaufender Serial-Logger liest esptool die Antworten weg – vor dem Upload beenden.
  pkill -f '[t]ools/serial_log.py' && sleep 1 || true
  PORT=$(ls /dev/ttyACM* 2>/dev/null | head -1)
  [ -n "$PORT" ] || { echo "kein ESP an /dev/ttyACM* gefunden" >&2; exit 1; }
  # Läuft schon eine TinyUSB-Firmware, muss der Upload über deren CDC-Port in den Bootloader wechseln.
  # Im Auslieferungszustand bzw. im Bootloader meldet sich der Chip als „USB JTAG/serial debug unit“
  # (gleiche VID:PID 303a:1001, deshalb am Produktnamen unterscheiden).
  MODE=cdc
  ls /dev/serial/by-id/ 2>/dev/null | grep -q 'JTAG' && MODE=default
  echo "Upload-Modus: $MODE"
  arduino-cli upload --fqbn "$FQBN,UploadMode=$MODE" --input-dir "$BUILD" -p "$PORT" "$DIR/$SKETCH"
fi

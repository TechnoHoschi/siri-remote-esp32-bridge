#!/usr/bin/env python3
"""Holt einen ESP32-S3 zurück, dessen Firmware beim Start abstürzt (Boot-Schleife), ohne BOOT-Taste.

Direkt nach dem Einstecken meldet sich der Chip für einen Moment als „USB JTAG/serial debug unit“,
bevor die Firmware den USB-Port übernimmt. Das Skript wartet auf genau diesen Moment und schickt sofort
die Reset-Sequenz in den Download-Modus. Bleibt der Chip danach als JTAG-Gerät stehen, wird geflasht.

Aufruf (als root, damit keine Wartezeit auf udev-Rechte entsteht):
  sudo .venv/bin/python -u tools/esp_rescue.py <esptool write-flash Argumente …>
Dann den ESP abziehen und wieder einstecken (ggf. mehrmals).
"""
import glob
import os
import subprocess
import sys
import time

import serial

PORT = "/dev/ttyACM0"
BY_ID = "/dev/serial/by-id/*JTAG*"
ESPTOOL = os.path.join(os.path.dirname(sys.executable), "esptool")
deadline = time.time() + 600


def into_download_mode() -> None:
    # Reset-Sequenz des USB-Serial/JTAG-Controllers (wie esptool, nur mit kürzeren Pausen)
    with serial.Serial(PORT, 115200, timeout=0.2) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.02)
        ser.dtr = True
        ser.rts = False
        time.sleep(0.02)
        ser.rts = True
        ser.dtr = False
        ser.rts = True
        time.sleep(0.02)
        ser.dtr = False
        ser.rts = False


attempt = 0
print("warte auf den ESP (abziehen und wieder einstecken) …", flush=True)
while time.time() < deadline:
    if not os.path.exists(PORT):
        time.sleep(0.002)
        continue
    attempt += 1
    try:
        into_download_mode()
    except (serial.SerialException, OSError) as e:
        print(f"Versuch {attempt}: Port weg, bevor die Sequenz durch war ({type(e).__name__})", flush=True)
        time.sleep(0.05)
        continue
    # Im Download-Modus bleibt das JTAG-Gerät stehen; in der Boot-Schleife verschwindet es sofort wieder.
    time.sleep(2.5)
    if glob.glob(BY_ID):
        print(f"Versuch {attempt}: ESP steht im Download-Modus – flashe", flush=True)
        result = subprocess.run([ESPTOOL, "--chip", "esp32s3", "--port", PORT, "--baud", "460800"] + sys.argv[1:])
        sys.exit(result.returncode)
    print(f"Versuch {attempt}: Sequenz gesendet, ESP ist aber wieder weg", flush=True)
print("Zeit abgelaufen, ESP nicht erwischt")
sys.exit(1)

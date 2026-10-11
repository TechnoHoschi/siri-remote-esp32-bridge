# Siri Remote ESP32 Bridge

[🇩🇪 Deutsch](README.md) · 🇬🇧 English

A few-dollar ESP32-S3 that pairs with the **Apple Siri Remote (3rd generation, USB-C)** over Bluetooth
and shows up on your media player as a perfectly ordinary **USB keyboard**. Built for **Kodi** on
**CoreELEC** or **LibreELEC**, but it works wherever a keyboard works.

In short: a "Flirc for the Siri Remote". Apple would rather sell you an Apple TV.

![Web interface overview](docs/uebersicht.png)

*The web interface is German-only for now. A language switch is on the wish list.*

## Why this exists

The Siri Remote may be the nicest remote control money can buy, and it talks to exactly one device on
this planet. Over infrared it can do volume and power. Everything else goes over Bluetooth LE in a
dialect that Android, Fire OS and Windows politely ignore.

This dongle speaks that dialect and translates it into the one thing every device has understood since
1996: key presses.

## What it does

- Pairs the remote by itself as soon as you hold it nearby in pairing mode
- All 13 buttons as USB keyboard or media keys, freely assignable
- Four mapping profiles to switch between and rename, three of them pre-filled
- No noticeable delay; holding a button repeats (the media player takes care of that)
- Web interface with live view of button presses, remote battery level, light and dark mode
- Firmware updates through the web interface, with automatic rollback if the update was nonsense
- A status LED that actually means something
- Backup and restore of the configuration as a JSON file

## What it does not do

- **Touchpad swipes.** The remote sends the data; so far we only use the clicks. Top of the wish list.
- **Siri.** The microphone is never subscribed to. Nobody is listening. Promise.
- **Fire TV.** Should work, but is untested; see the [wish list](#wish-list).
- **Passwords.** More on that below.

## What it was tested with

Honestly: exactly one setup.

| Part | Tested with | Note |
|---|---|---|
| Board | ESP32-S3 Super Mini (4 MB flash, 2 MB PSRAM) | Other ESP32-S3 boards with a native USB port probably work too; the LED will sit on a different pin (configurable). |
| Remote | Siri Remote 3rd generation, model A2854 (USB-C) | Older generations speak a different protocol. |
| Media player | Kodi on CoreELEC | Anything that accepts a USB keyboard should work. |
| Cable | USB-C | One that carries data. Yes, that needs saying. |

If you try it with something else, feedback is welcome, either way.

## Installing

**The comfortable way:** plug the ESP into your computer, open the
[browser installer](https://technohoschi.github.io/siri-remote-esp32-bridge/) in Chrome or Edge, click
"Installieren".

**The manual way:** grab `siri_bridge-…-merged.bin` from [Releases](../../releases) and flash it to
address `0x0` with [esptool-js](https://espressif.github.io/esptool-js/) or `esptool`.

If the ESP does not enter flash mode by itself: hold BOOT, tap RESET, release BOOT.

Both ways overwrite everything, including pairing and settings. For later updates, use the `…-ota.bin`
and the web interface instead.

## First steps

1. Plug the dongle into the media player. The LED blinks fast: it is looking for a remote.
2. Hold the remote right next to the dongle and press **Back + Volume Up** for about five seconds.
3. The LED turns solid. Done. That was the whole setup.

If the remote is still paired with an Apple TV, switch that off first. It can only be in one
relationship at a time.

### The status LED

| LED | Meaning |
|---|---|
| Fast blinking (blue) | No remote paired |
| A flash every 2 seconds (blue) | Paired; the remote is asleep and will connect on the next key press |
| Solid (blue) | Connected |
| Slow blinking (violet) | Own hotspot is open: set up Wi-Fi |
| Flickering (white) | Firmware update in progress, hands off the plug |
| Three blinks (yellow) | Wi-Fi was just switched on or off |

If a solid light behind the TV is too much of a light show, the settings let you turn the LED off
while connected.

### Mapping and profiles

The dongle ships with four profiles that you switch between in the web interface. Each can be remapped
and renamed.

| Remote | Universal (default) | Kodi (CoreELEC) | Android / Fire TV |
|---|---|---|---|
| Ring up / right / down / left | Arrow keys | Arrow keys | Arrow keys |
| Clickpad centre | Enter | Enter | Enter |
| Back | Media "Back" | Backspace | Media "Back" |
| Home/TV | Menu key | C (context menu) | Media "Home" |
| Play/Pause | Media "Play/Pause" | Space | Media "Play/Pause" |
| Power | S (shutdown menu in Kodi) | S | nothing |
| Volume up / down / Mute | Media keys | Media keys | Media keys |
| Siri | nothing | nothing | nothing |

The fourth profile, "Eigenes Profil" (custom), starts as a copy of "Universal" for tinkering.

"Universal" was copied from a remote that works on both Fire TV and CoreELEC through a Flirc dongle: the
keyboard's Menu key and the media "Back" key. If you have such a proven pair yourself, `tools/key_scan.py`
shows what it sends. "Android / Fire TV" is untested so far.

## The web interface

On first start the dongle opens its own network called `siri-bridge-setup`. Connect, the page opens
by itself (otherwise `http://192.168.4.1`), pick your home network, enter the password. After that it
joins your network and stays reachable there. If it cannot find it, the hotspot returns after about
15 seconds. The hotspot itself switches off after ten minutes without access.

A short press on **BOOT** on the dongle, or five seconds on the **Siri button**, turns Wi-Fi off and
on again; the LED acknowledges with three yellow blinks. Finally the Siri button has a job. If you do
not want Wi-Fi running permanently, set a switch-off time in the settings. You can assign a static IP
address there as well.

The BOOT button can do more, each on release:

| Hold BOOT | LED meanwhile | Effect |
|---|---|---|
| short press | – | Wi-Fi on/off |
| 3 seconds | blinking red | Unpair the remote |
| 10 seconds | white | Factory reset: pairing, settings and Wi-Fi gone |

The last one is the emergency exit if a wrong IP address has locked you out.

![Button mapping](docs/tasten.png)

Under **Tasten** (buttons) you click a button on the drawn remote and choose what it sends. If you have
the real remote in your hand, just press the button; the page jumps to it.

![Settings](docs/einstellungen.png)

Under **Einstellungen** (settings) you find LED, connection, Wi-Fi, firmware update, backup, and three
levels of "undo everything": forget Wi-Fi, reset settings, factory reset.

### About passwords

There are none. Not for the hotspot, not for the web interface. That is on purpose.

This thing is a remote control bridge, not a vault. Anyone on the same network can remap your volume
button or upload different firmware. If you consider that a realistic scenario in your own living
room, set "turn Wi-Fi off automatically" to a few minutes, and the web interface only exists at the
push of a button. Everyone else can keep a sense of proportion.

## Updates and rollback

Firmware updates arrive as `…-ota.bin` through the web interface. Pairing and settings survive.

If new firmware crashes within its first 15 seconds, the bootloader goes back to the previous version
on the next attempt. This feature exists because an earlier version crashed on startup and the dongle
then stopped showing up on USB. One learns.

Should nothing work at all any more: `tools/esp_rescue.py` catches the ESP in the brief moment after
plugging in when it can still be talked to. That, too, comes from experience.

## Building it yourself

Development happens on a Raspberry Pi 5 with the ESP plugged in via USB. Handy: the Pi sees the ESP as
a keyboard and can check the whole chain.

```bash
firmware/build.sh siri_bridge          # build; images end up in firmware/dist/
firmware/build.sh siri_bridge upload   # build and flash
python3 tools/ui_dev.py                # web interface without an ESP, with sample data, port 8766
```

You need `arduino-cli`, the ESP32 core 3.3.12 and the NimBLE-Arduino library 2.5.1. The core takes
about 8 GB. For a remote control.

On every version tag, GitHub Actions builds the images, attaches them to the release and updates the
browser installer.

| Where | What |
|---|---|
| `firmware/siri_bridge/` | Firmware: BLE, USB HID, web interface, settings |
| `firmware/siri_bridge/web/index.html` | The web interface, one single file |
| `tools/ui_dev.py` | Development server for the web interface |
| `tools/serial_log.py`, `tools/hid_watch.py` | Read the ESP's serial log and its USB key presses on Linux |
| `tools/key_scan.py` | Shows what another USB input device sends, a Flirc for example |
| `tools/esp_rescue.py` | Gets an ESP out of a boot loop |
| `tools/linux/` | Read the remote directly on a Linux machine (from the exploration phase; needs BlueZ and `bleak`) |
| `docs/PROTOCOL.md` | What we found out about the remote's protocol |

Code comments and log messages are in German.

## What is different under the hood from what you read elsewhere

For everyone who has already broken a tooth on this remote:

- **No enable write needed.** After bonding and subscribing, the reports simply arrive.
- **The connection parameters are the crux.** The remote wants a 7.5–15 ms interval, latency 80 and a
  6 s timeout. With defaults the link drops every few seconds.
- **Two ways of announcing itself.** In pairing mode with a recognisable fingerprint, after waking up
  anonymously. Recognising it then only works through the IRK-resolved address.
- **It buffers key presses** while disconnected and delivers them afterwards.
- **The BlueZ plugin is called `hog`**, not `hogp`, and there is no `DisablePlugins` in `main.conf`.

Details are in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Wish list

- [ ] **Touchpad:** swipes as arrow keys, like on an Apple TV
- [ ] Test the **Fire TV Cube** and other Android devices, then refine the profiles
- [ ] **Long press** as a second mapping
- [ ] **Sending infrared** as a mapping type, for TV volume for instance (needs an IR LED on the dongle)
- [ ] **Receiving infrared:** translate ordinary IR remotes as well, the dongle as a better Flirc
- [ ] **Switchable language** for the web interface
- [ ] Properly test the mobile view of the web interface

## Thanks

Without the groundwork of these projects this would have taken considerably longer:

- [azais-corentin/siri-remote](https://github.com/azais-corentin/siri-remote) – protocol of the 3rd generation
- [Yanndroid/SiriRemote-Linux](https://github.com/Yanndroid/SiriRemote-Linux) (branch `gen-3`) – the button bit mask
- [zanneth/siriremote](https://code.zanneth.com/zanneth/siriremote) – Linux client
- [h2zero/NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) – the BLE stack

Built over two days in dialogue with Claude Code, which sent the ESP into a boot loop once along the
way and then got it back out again.

## License

[MIT No Attribution](LICENSE): do whatever you like with it, no credit required. The prebuilt firmware
images additionally contain the ESP32 Arduino core (LGPL-2.1) and NimBLE-Arduino (Apache-2.0), which
remain under their own licenses.

Apple, Siri and Apple TV are trademarks of Apple Inc. This project has nothing to do with Apple, other
than being rather fond of one of their remotes.

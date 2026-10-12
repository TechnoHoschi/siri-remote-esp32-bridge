# Siri Remote (3rd generation) over BLE – protocol notes

What we learned about the Apple Siri Remote, 3rd generation (model A2854, USB-C, Bluetooth device ID
`004C:0315`), while building this bridge. Everything marked ✅ was observed on our own remote, first with
BlueZ 5.82 on a Raspberry Pi 5 (HCI traces via `btmon`), then with NimBLE on an ESP32-S3. One remote, one
firmware version – treat it as a well-documented data point, not as a specification.

Several findings contradict what other projects describe. Those are marked ⚠️.

## Pairing

- ✅ Hold **Back + Volume Up** for about 5 seconds. The remote then advertises in pairing mode for
  roughly 20 seconds.
- ✅ Pairing is **Just Works** with LE Legacy Pairing and bonding. The remote distributes an LTK and an
  IRK. Its identity address is a public address.
- ✅ The remote uses resolvable private addresses and rotates them, so a host must bond and resolve the
  address through the IRK to find it again.
- ✅ Outside pairing mode the remote still answers a Pairing Request with a Pairing Response and then
  disconnects about 250 ms after the host's Pairing Confirm (HCI reason `0x13`). Its advertising looks
  the same in both cases, so a host cannot tell beforehand – it has to try, and should back off between
  attempts.
- ✅ With BlueZ, `bluetoothctl --agent NoInputNoOutput pair <address>` works. No custom agent needed.
- Our first attempt failed at −67 dBm with agent capability `DisplayYesNo`; the second succeeded at
  −43 dBm with `NoInputNoOutput`. We changed both at once, so we do not know which one mattered. Keep
  the remote close.

## Two advertising formats ⚠️

The remote announces itself in two different ways:

| When | Advertising data |
|---|---|
| Pairing mode, and right after a connection loss | Appearance `0x03C0`, HID service `0x1812`, Apple manufacturer data `07 0D 02 15 03 02 <identity address, 6 bytes> 48 49 4A` |
| Bonded remote waking up on a key press | Only Apple manufacturer data type `0x07`, length `0x11`, 17 changing bytes starting with `08`. No HID service, no appearance, no identity address |

A host that only filters for the first format pairs fine and then never finds the remote again after its
own reboot. The second format can only be matched through the resolved address. On the ESP32-S3 the
controller resolves it, and NimBLE reports the identity address in scan results.

The trailing bytes of the first format differ from what `azais-corentin/siri-remote` documents
(`4F 50 50`); we do not know what they encode.

## Connection parameters – the part that matters most

- ✅ The remote's Peripheral Preferred Connection Parameters (`0x2A04`) ask for an interval of
  **7.5–15 ms, peripheral latency 80, supervision timeout 6 s**.
- ✅ With typical host defaults (we saw 48.75 ms, latency 0, timeout 420 ms on Linux) the link dies with
  a connection timeout every few seconds, as soon as the remote sleeps between key presses.
- ✅ With 15 ms / 80 / 6 s the link stays up while idle. We saw 18 minutes without input and without a
  single drop; a key press after that pause arrived over the existing connection, with no visible delay.
- On Linux: set `MinConnectionInterval=6`, `MaxConnectionInterval=12`, `ConnectionLatency=80`,
  `ConnectionSupervisionTimeout=600` in the `[LE]` section of `/etc/bluetooth/main.conf`. A per-device
  `[ConnectionParameters]` entry in BlueZ's `info` file was loaded but not used by the kernel on connect.

## No enable write needed ⚠️

Other projects state that the remote stays silent until the host writes a magic value (`0xAF` to every
HID output report, or `F0 00` to a specific handle).

- ✅ On our remote **no such write is needed**. After bonding, encryption and subscribing to the
  notifications, button, touch and microphone reports arrive. In the complete HCI trace the only writes
  to the remote were CCCD writes.
- ✅ Our remote has **no HID output report** at all, so "write `0xAF` to every output report" has no
  target.
- The `F0 00` variant targets the feature report `0xF0`. We never needed it and did not test it against
  the remote.

## HID reports

All report characteristics share UUID `0x2A4D`. They can only be told apart through their Report
Reference descriptor (`0x2908`), so never hard-code handles.

| Report ID | Type | Content |
|---|---|---|
| `0xFA` | Input | Microphone audio (Opus), only while the Siri button is held |
| `0xFB` | Input | ✅ Buttons: 2-byte little-endian bit mask |
| `0xFC` | Input | ✅ Touchpad: 11 bytes for one finger, 18 bytes for two (see below) |
| `0x60`, `0xFD`, `0xFE` | Input | Unknown, no data seen |
| `0xF0`, `0xF1` | Feature | Unknown |

### Button mask (`0xFB`) ✅

All 13 buttons verified one by one. The remote reports changes only: one report on press, one on release.
There is no repeat while a button is held.

| Bit | Button | Bit | Button |
|---|---|---|---|
| `0x0001` | Home/TV | `0x0080` | Mute |
| `0x0002` | Volume up | `0x0100` | Play/Pause |
| `0x0004` | Volume down | `0x0200` | Ring up |
| `0x0008` | Clickpad centre | `0x0400` | Ring right |
| `0x0010` | Power | `0x0800` | Ring down |
| `0x0020` | Siri | `0x1000` | Ring left |
| `0x0040` | Back | | |

Button reports arrive on a 15 ms grid, matching the connection interval.

### Buffered key presses ✅

While it is not connected, the remote buffers key presses and delivers them after the next connection –
several of them, even from hours earlier. They arrive as a burst right after connecting, press and
release only about 15 ms apart. A bridge has to decide whether to drop them.

### Touchpad (`0xFC`)

Measured on the ESP32-S3 with one finger, sweeping the whole surface (322 reports). The decoding below
gives smooth tracks without outliers; it follows the layout of Apple's Magic Mouse touch data. Bytes 0–3
(first byte always `0x32`, then a counter-like value) and the two-finger layout are not decoded yet.

| Bytes | Content |
|---|---|
| 4, 5 (low nibble) | X, 12 bit signed: `x = b4 \| (b5 & 0x0F) << 8`; left ≈ −960, right ≈ +320 |
| 5 (high nibble), 6 | Y, 12 bit signed: `y = b5 >> 4 \| b6 << 4`; **up is positive**: bottom ≈ −985, top ≈ +290 |
| 7, 8 | Presumably contact size (major / minor axis); not needed for tracking |
| 10 (low 3 bits) | State: 3 = touch down, 4 = touching, 5–7 = lifting, 0 = gone |

- The usable range is about 1280 units on each axis. The origin is not in the middle: a tap on the centre
  reads roughly (−285, −365). For relative movement only the differences matter.
- While a finger rests on the pad, one report arrives per connection interval: every 15 ms, about 66 per
  second. Between touches nothing is sent.
- A short tap without clicking still produces 9–13 reports, so touch down and lift are easy to tell apart.
- Two fingers produce 18-byte reports; the second finger presumably follows in the same 7-byte layout.
- The surface is the inner disc and the ring around it; clicks on the ring are separate button bits.

## Other GATT bits

- Battery service `0x180F`: battery level `0x2A19` ✅ (readable, notifies).
- `0x2A1A` in the same service is a power state. Other projects list `0xAB` charging, `0xAF`
  discharging, `0xBB` plugged in; on battery we read `0x6F`. ⚠️
- The device name is the remote's serial number.
- Infrared: the remote sends volume up/down and mute over IR by itself, with codes it learned for the TV
  while paired to an Apple TV. After pairing with the dongle this kept working on an LG TV most of the
  time, but was seen not working once in between. We do not know how the Apple TV configures it or what
  makes the remote keep or drop it.

## BlueZ specifics

- ⚠️ The plugin that claims HID-over-GATT devices is called **`hog`**, not `hogp`, and BlueZ 5.82 has no
  `DisablePlugins=` option in `main.conf`. Disable it with a systemd drop-in that starts
  `bluetoothd -P hog`; the journal then says `Excluding (cli) hog`.
- With `hog` active, BlueZ takes the remote as a `hidraw` device, subscribes to the reports itself and
  rejects writes to the report characteristics locally.
- After pairing, the D-Bus object path keeps the random address until `bluetoothd` restarts. Look the
  device up by its `Address` property instead of building the path.
- Controllers without LL Privacy (for example the BCM4345C0 in the Raspberry Pi 5) cannot filter for a
  bonded device with a rotating address. Scan until connected, then stop scanning immediately – a
  continuous scan starves the connection into a supervision timeout.

## ESP32-S3 / NimBLE specifics

- NimBLE-Arduino 2.5 as central: bonding, IRK resolution and the connection parameters above work.
- A reconnect takes about 0.7 s: roughly 230 ms to connect, 150–165 ms for encryption, 300 ms for
  discovery and subscribing.
- From receiving a button report to handing it to USB takes about 0.9 ms.
- USB hosts generate key repeat themselves while a key is held; the bridge does not need its own.

## Thanks

These projects did the groundwork this builds on:

- [azais-corentin/siri-remote](https://github.com/azais-corentin/siri-remote)
- [Yanndroid/SiriRemote-Linux](https://github.com/Yanndroid/SiriRemote-Linux), branch `gen-3`
- [zanneth/siriremote](https://code.zanneth.com/zanneth/siriremote)

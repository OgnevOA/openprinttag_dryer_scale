# Building and flashing

## Toolchain

| Item | Version used | Notes |
| --- | --- | --- |
| Arduino IDE | 2.3.x | 1.8.19 also works; the sketch uses no IDE 2 features |
| ESP32 board package | 3.3.11 | Compiled against this one; 3.0.0 is the earliest with ESP32-C6 support |
| Board selection | `XIAO_ESP32C6` | Seeed Studio XIAO ESP32C6 |
| PubSubClient | 2.8.0 | Library Manager, and the only library to install |

Arduino pins nothing for you, so record what you actually installed. The versions
above are not a guess at what should work: the firmware was last compiled clean
against exactly these, so if a future IDE or core update breaks the build, this
is the combination to fall back to.

### Adding the board package

1. File > Preferences > Additional boards manager URLs, add:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. Tools > Board > Boards Manager, search `esp32`, install
   **esp32 by Espressif Systems** 3.3.11 or newer.
3. Tools > Board > ESP32 Arduino > **XIAO_ESP32C6**.

### Board settings

| Setting | Value | Why |
| --- | --- | --- |
| USB CDC On Boot | Enabled | Serial logs over the XIAO's native USB |
| Partition Scheme | Leave at Default | `partitions.csv` in the sketch folder overrides it with 1.9 MB OTA slots |
| Flash Size | 4 MB (32 Mb) | XIAO ESP32-C6 |
| CPU Frequency | 160 MHz | Default |
| Core Debug Level | None (Warn while bringing hardware up) | |
| Erase All Flash Before Sketch Upload | Disabled | Keeps calibration in NVS across flashes |

That last one matters: calibration constants and Wi-Fi credentials live in NVS,
and a full erase throws away the dryer tare.

### Libraries

Library Manager, exactly one:

- **PubSubClient** by Nick O'Leary

Everything else ships with the ESP32 core (`WiFi`, `WebServer`, `Preferences`,
`ArduinoOTA`, `ESPmDNS`, `SPI`, `Wire`, `Update`) or lives in the sketch itself.
The CBOR codec and the HX711 driver are deliberately in-project rather than
pulled from the Library Manager: the codec has to be testable on a desktop, and
the HX711 protocol is short enough that owning it beats depending on it.

No JSON library either. Both documents this firmware produces — the MQTT state
payload and the discovery configs — have a fixed shape and are assembled with
`snprintf`, which costs a few hundred lines and saves the flash for a second OTA
slot. It also means `HaMqtt::buildStateJson` is a pure function the desktop tests
can check byte for byte.

## Sketch layout

Arduino compiles the sketch folder plus everything under `src/` recursively, so
the whole firmware is one sketch directory with no manual library installation:

```
firmware/OpenPrintTagDryer/
  OpenPrintTagDryer.ino
  config.h                 pin map, timings, thresholds
  secrets.h                git-ignored, copied from secrets.example.h
  src/openprinttag/        tag codec (no Arduino dependencies)
  src/pn5180/              ISO 15693 reader driver
  src/*.h, src/*.cpp       managers: NFC, scale, spool state, MQTT, web portal
```

## First flash

1. Copy `secrets.example.h` to `secrets.h`. It can stay empty: the device
   raises its own access point (`openprinttag-dryer` / `printtag`) when it has
   no Wi-Fi credentials.
2. Plug the XIAO into USB. It appears as `/dev/ttyACM0` on Linux.
3. Select the port and upload.
4. Open Serial Monitor at 115200. The banner reports the firmware version and
   the result of each subsystem's startup probe.

If the board does not appear, hold BOOT, tap RESET, release BOOT to force the
ROM bootloader, then upload once; afterwards USB CDC handles resets on its own.

## Flash budget

The XIAO ESP32-C6 has 4 MB of flash, and its board definition offers exactly one
OTA-capable partition scheme: 1.2 MB per app slot. This firmware is about
1.28 MB, which fits with no margin worth the name. `partitions.csv` in the sketch
folder therefore replaces the stock table with 1.9 MB slots, paid for by the
filesystem the firmware does not use — the web pages are compiled in and the
settings live in NVS. The board package copies that file into the build
automatically, so what the Partition Scheme menu says does not matter.

One consequence to know about, because it looks like a problem and is not:

```
Sketch uses 1283064 bytes (97%) of program storage space. Maximum is 1310720 bytes.
```

That percentage is measured against the *menu's* scheme, not the table actually
being flashed. Against the real 1856 kB slot it is 67%, which you can confirm for
yourself: the generated `partitions.bin` next to the build output has `app0` at
`0x1D0000` bytes. The number to watch is the byte count rather than the
percentage, because while it stays under 1310720 the IDE uploads happily and
stops the moment it does not — and as of the Spoolman client there is only about
27 kB of that headroom left, even though the partition being flashed into has
570 kB spare.

That day is close, so: selecting **No OTA (2MB APP/2MB SPIFFS)** raises the ceiling
the IDE checks against without changing what gets flashed, since `partitions.csv`
still wins and still has two OTA slots. It is a menu setting chosen for its size
check, not for its partition layout, which is worth a comment next to whoever has
to understand it later. Dropping features is the other option, and dropping OTA
for real is the last one.

## Desktop tests

The codec and every logic module build and run on a PC, so encoding, decoding,
the weight maths, the write-back policy, the MQTT documents and the web portal's
forms are all proven before any hardware is involved:

```bash
cd tests
make test        # 2200+ assertions, codec fixtures plus firmware logic
make asan        # same, under sanitizers (needs libasan/libubsan)
```

The logic tests run against simulated hardware: an HX711 that answers on the
bit-banged pins, an I2C temperature sensor, an unresponsive PN5180, an MQTT
client that records what was published, and a web server that dispatches
requests. Two consequences worth knowing: a failing assertion names the
behaviour rather than a line number, and code that only works because real
hardware is forgiving will fail here.

`tests/fixtures/` holds the binary tag images and reference decodes published
by the OpenPrintTag specification. `tests/fixture_expectations.h` and
`src/openprinttag/opt_enums.h` are generated from those and from
`spec-data/*.yaml`; regenerate with:

```bash
python3 tools/gen_enums.py
python3 tools/gen_fixture_expectations.py
```

## OTA updates

Once the device is on Wi-Fi it appears in the IDE's port list as a network port
(`openprinttag-dryer at <ip>`). Uploading over it prompts for the OTA password
from `secrets.h`. The web portal at `http://openprinttag-dryer.local/` also
accepts a compiled `.bin` directly, which is the more reliable path when the
IDE's mDNS discovery is being difficult.

# OpenPrintTag Dryer Reader/Writer with Scale

[![CI](https://github.com/OgnevOA/openprinttag_dryer_scale/actions/workflows/ci.yml/badge.svg)](https://github.com/OgnevOA/openprinttag_dryer_scale/actions/workflows/ci.yml)

A XIAO ESP32-C6 device that turns a Creality Space Pi filament dryer into a smart,
self-tracking spool holder:

- **Reads and writes OpenPrintTag** (ISO 15693 / NFC-V, ICODE SLIX2) tags through the
  side lid of the dryer using a PN5180 NFC frontend.
- **Weighs the whole dryer** on an HX711 + 5 kg load cell underneath it, and derives the
  remaining filament from the tag's own per-spool reference weights.
- **Writes `consumed_weight` back to the tag**, so the spool stays accurate offline for
  any other OpenPrintTag reader (phone app, printer, another device).
- **Provisions blank tags** for older QR-code-era Prusament spools, using the exact
  per-spool weights published at `prusament.com/spool/?spoolId=<id>`.
- **Integrates with Home Assistant** over MQTT discovery: no custom integration needed.
- **Corrects Spoolman from the scale**, optionally: a spool's remaining weight there is
  otherwise only ever a printer's estimate, and this replaces it with a measurement.

## Repository layout

| Path | Contents |
| --- | --- |
| `firmware/OpenPrintTagDryer/` | Arduino IDE sketch (ESP32-C6) |
| `tests/` | Desktop (g++) test harness for the tag codec + spec fixtures |
| `hardware/` | Carrier PCB design, netlist, BOM, wiring |
| `mechanical/` | Parametric sources (Python/trimesh) and STLs for the lid bracket and scale sandwich |
| `homeassistant/` | Dashboard, automations, MQTT notes |
| `deploy/` | Spoolman as a TrueNAS SCALE custom app |
| `docs/` | Build, assembly, calibration, provisioning, tag format, validation |
| `tools/` | Generators for the spec-derived tables, the drift-log fitter, the package checker |

## Quick start

1. Read [docs/build.md](docs/build.md) and install the Arduino IDE board package and
   the one required library.
2. Copy `firmware/OpenPrintTagDryer/secrets.example.h` to `secrets.h` and fill it in
   (or leave it blank and use the on-device Wi-Fi portal).
3. Wire the hardware per [docs/assembly.md](docs/assembly.md).
4. Flash, then run the on-device self test at `http://<device>/selftest`.
5. Calibrate the scale with [docs/calibration.md](docs/calibration.md).
6. Provision your spools with [docs/provisioning.md](docs/provisioning.md).
7. Connect it to Home Assistant with
   [homeassistant/README.md](homeassistant/README.md), and Spoolman, if you run it,
   with [docs/spoolman.md](docs/spoolman.md).
8. Work through [docs/validation.md](docs/validation.md) before trusting any
   number it reports.

## Documentation

| Document | What it covers |
| --- | --- |
| [docs/build.md](docs/build.md) | Board package, libraries, IDE settings, flash budget, OTA |
| [docs/assembly.md](docs/assembly.md) | Bench bring-up, the scale sandwich, the reader at the lid |
| [docs/calibration.md](docs/calibration.md) | Zero, span, tare, the sealed-spool cross-check, drift compensation |
| [docs/provisioning.md](docs/provisioning.md) | Turning a QR-code spool into a tagged one |
| [docs/validation.md](docs/validation.md) | Eight acceptance tests, and what "good" looks like |
| [docs/tag-format.md](docs/tag-format.md) | What the codec writes, and the update rules it honours |
| [docs/spoolman.md](docs/spoolman.md) | Linking a tag to a spool, and what protects Spoolman's data |
| [hardware/README.md](hardware/README.md) | The carrier board, its layout rules, the ribbon |
| [mechanical/README.md](mechanical/README.md) | The printed parts, the stack, print settings |
| [homeassistant/README.md](homeassistant/README.md) | Broker, package, dashboard, blueprint, entity reference |

## Running the tests

The codec and every logic module build with no Arduino dependency, so they run on
the desktop against the fixtures published by the OpenPrintTag specification:

```bash
cd tests
make test        # 2200+ assertions across the codec and the firmware logic
```

Three more, for the artefacts the device produces:

```bash
make -C tests dump_tag && tests/dump_tag tag.bin  # a tag image, decoded
python3 tools/check_ha_package.py                 # entity ids and YAML syntax
python3 tools/fit_drift.py drift.csv              # drift, with the legs separated
```

`tag.bin` comes from **Download these bytes** on the portal's Tag page, which
makes any tag — including one written by another tool — something the codec can
be checked against.

## Standards and credits

- [OpenPrintTag specification](https://github.com/OpenPrintTag/openprinttag-specification)
  by Prusa Research (MIT). The field and enum definitions in `spec-data/` and the test
  fixtures in `tests/fixtures/` come from that repository.
- PN5180 ISO 15693 driver derived from
  [ATrappmann/PN5180-Library](https://github.com/ATrappmann/PN5180-Library) (LGPL-2.1),
  kept isolated in `firmware/OpenPrintTagDryer/src/pn5180/`.

## Licence

MIT, except `firmware/OpenPrintTagDryer/src/pn5180/` which remains LGPL-2.1.
See [LICENSE](LICENSE).

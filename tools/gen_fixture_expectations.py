#!/usr/bin/env python3
"""Generate tests/fixture_expectations.h from the specification's test fixtures.

The OpenPrintTag repository ships binary tag images alongside `*_info.yaml`
files holding the reference implementation's decode of each image. Those YAML
files are the golden reference: this script turns them into a C++ table so the
desktop tests compare the firmware's decoder against Prusa's own output rather
than against hand-written expectations.

    python3 tools/gen_fixture_expectations.py
"""

from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "tests" / "fixtures"
SPEC = ROOT / "spec-data"
OUT = ROOT / "tests" / "fixture_expectations.h"

# Fields the firmware decodes, mapped to the comparison kind the test uses.
FIELD_KINDS = {
    "material_class": "Enum",
    "material_type": "Enum",
    "material_name": "Text",
    "material_abbreviation": "Text",
    "brand_name": "Text",
    "brand_specific_instance_id": "Text",
    "gtin": "Int",
    "manufactured_date": "Int",
    "expiration_date": "Int",
    "nominal_netto_full_weight": "Float",
    "actual_netto_full_weight": "Float",
    "empty_container_weight": "Float",
    "nominal_full_length": "Float",
    "actual_full_length": "Float",
    "primary_color": "Color",
    "density": "Float",
    "filament_diameter": "Float",
    "min_print_temperature": "Int",
    "max_print_temperature": "Int",
    "preheat_temperature": "Int",
    "min_bed_temperature": "Int",
    "max_bed_temperature": "Int",
    "min_chamber_temperature": "Int",
    "max_chamber_temperature": "Int",
    "chamber_temperature": "Int",
    "drying_temperature": "Int",
    "drying_time": "Int",
    "container_width": "Int",
    "container_outer_diameter": "Int",
    "container_inner_diameter": "Int",
    "container_hole_diameter": "Int",
    "write_protection": "Enum",
    "tags": "TagList",
}

AUX_FIELD_KINDS = {
    "consumed_weight": "Float",
    "workgroup": "Text",
    "storage_location": "Text",
    "purchase_time": "Int",
    "purchase_price": "Float",
    "purchase_currency": "Text",
}


def load_yaml(path):
    with path.open() as handle:
        return yaml.safe_load(handle)


def field_keys(filename):
    """Maps field name to numeric key from the specification's definitions."""
    entries = load_yaml(SPEC / filename)
    return {entry["name"]: entry["key"] for entry in entries if "name" in entry}


def enum_lookup(filename):
    """Maps every label a value may appear under to its numeric key."""
    entries = load_yaml(SPEC / filename)
    mapping = {}
    for entry in entries:
        for label_key in ("name", "abbreviation"):
            label = entry.get(label_key)
            if label is not None:
                mapping[str(label)] = entry["key"]
    return mapping


def cbor_map_entries(data):
    """Returns [(key, major, value_bytes)] for the CBOR map at the start of `data`.

    A deliberately small CBOR walker: enough to skip any item the specification
    can produce, so the generator can tell how many fields the raw region holds
    versus how many the reference decoder recognised, and can read a value the
    _info.yaml files do not spell out.
    """

    def read_head(pos):
        initial = data[pos]
        major = initial >> 5
        info = initial & 0x1F
        if info < 24:
            return major, info, info, pos + 1
        if info == 24:
            return major, info, data[pos + 1], pos + 2
        if info == 25:
            return major, info, int.from_bytes(data[pos + 1:pos + 3], "big"), pos + 3
        if info == 26:
            return major, info, int.from_bytes(data[pos + 1:pos + 5], "big"), pos + 5
        if info == 27:
            return major, info, int.from_bytes(data[pos + 1:pos + 9], "big"), pos + 9
        if info == 31:
            return major, info, None, pos + 1
        raise ValueError(f"reserved additional info {info}")

    def skip(pos):
        major, info, argument, nxt = read_head(pos)
        if major in (0, 1):
            return nxt
        if major in (2, 3):
            if info == 31:
                while data[nxt] != 0xFF:
                    nxt = skip(nxt)
                return nxt + 1
            return nxt + argument
        if major in (4, 5):
            per_entry = 2 if major == 5 else 1
            if info == 31:
                while data[nxt] != 0xFF:
                    nxt = skip(nxt)
                return nxt + 1
            for _ in range(argument * per_entry):
                nxt = skip(nxt)
            return nxt
        if major == 6:
            return skip(nxt)
        if major == 7:
            return nxt
        raise ValueError(f"unhandled major type {major}")

    major, info, argument, pos = read_head(0)
    if major != 5:
        return []

    entries = []

    def read_entry(pos):
        key_major, _, key_argument, after_key = read_head(pos)
        key = key_argument if key_major == 0 else None
        value_major, _, _, _ = read_head(after_key)
        after_value = skip(after_key)
        entries.append((key, value_major, data[after_key:after_value]))
        return after_value

    if info == 31:
        while data[pos] != 0xFF:
            pos = read_entry(pos)
        return entries
    for _ in range(argument):
        pos = read_entry(pos)
    return entries


def count_cbor_map_keys(data):
    return len(cbor_map_entries(data))


def cbor_byte_string(value_bytes):
    """The payload of a byte string, stripped of its own header."""
    initial = value_bytes[0]
    if initial >> 5 != 2:
        return None
    info = initial & 0x1F
    header = {24: 2, 25: 3, 26: 5, 27: 9}.get(info, 1)
    return value_bytes[header:]


def c_string(value):
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def build_fields(section_data, name_to_key, kinds, enums):
    fields = []
    recognised = 0
    for name, value in (section_data or {}).items():
        kind = kinds.get(name)
        if kind is None:
            continue
        recognised += 1
        key = name_to_key[name]
        integer = 0
        floating = 0.0
        text = ""

        if kind == "Int":
            integer = int(value)
        elif kind == "Float":
            floating = float(value)
        elif kind == "Text":
            text = str(value)
        elif kind == "Color":
            text = str(value).lstrip("#")
        elif kind == "Enum":
            table = enums[name]
            integer = table[str(value)]
        elif kind == "TagList":
            values = []
            for item in value:
                table = enums["tags"]
                values.append(table.get(str(item), item if isinstance(item, int) else 0))
            text = ",".join(str(item) for item in values)

        fields.append({
            "name": name,
            "key": key,
            "kind": kind,
            "int": integer,
            "float": floating,
            "text": text,
        })

    fields.sort(key=lambda item: item["key"])
    return fields, recognised


# The identity UUIDs, which the firmware carries so that re-encoding a tag does
# not discard them. Taken from the raw region rather than the _info.yaml `uuids`
# block: that block also holds values the reference checker *derived*, and what
# needs testing is what is actually stored.
UUID_KEYS = {
    "instance_uuid": 0,
    "package_uuid": 1,
    "material_uuid": 2,
    "brand_uuid": 3,
}


def build_uuid_fields(raw_main_hex):
    fields = []
    if not raw_main_hex:
        return fields, 0
    entries = {key: value for key, _, value in
               cbor_map_entries(bytes.fromhex(raw_main_hex))}
    for name, key in UUID_KEYS.items():
        value = entries.get(key)
        if value is None:
            continue
        payload = cbor_byte_string(value)
        if payload is None or len(payload) != 16:
            continue
        fields.append({
            "name": name,
            "key": key,
            "kind": "Uuid",
            "int": 0,
            "float": 0.0,
            "text": payload.hex(),
        })
    return fields, len(fields)


def main():
    main_keys = field_keys("main_fields.yaml")
    aux_keys = field_keys("aux_fields.yaml")
    enums = {
        "material_class": enum_lookup("material_class_enum.yaml"),
        "material_type": enum_lookup("material_type_enum.yaml"),
        "write_protection": enum_lookup("write_protection_enum.yaml"),
        "tags": enum_lookup("tags_enum.yaml"),
    }

    names = sorted(path.name[:-len("_info.yaml")]
                   for path in FIXTURES.glob("*_info.yaml"))

    out = [
        "// Generated by tools/gen_fixture_expectations.py -- do not edit.",
        "//",
        "// Expected values are the reference implementation's own decode of each",
        "// fixture, taken from the *_info.yaml files shipped with the",
        "// OpenPrintTag specification.",
        "",
        "#pragma once",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "namespace fixtures {",
        "",
        "enum class Kind { Int, Float, Text, Color, Enum, TagList, Uuid };",
        "",
        "struct Field {",
        "    const char *name;",
        "    uint16_t key;",
        "    Kind kind;",
        "    long long integer;",
        "    double floating;",
        "    const char *text;",
        "};",
        "",
        "struct Region {",
        "    int payloadOffset;",
        "    int absoluteOffset;",
        "    int size;",
        "};",
        "",
        "struct Fixture {",
        "    const char *name;",
        "    const char *path;",
        "    int ccCapacity;",
        "    int dataSize;",
        "    int payloadStart;",
        "    int payloadSize;",
        "    Region meta;",
        "    Region main;",
        "    Region aux;",
        "    bool hasAux;",
        "    bool hasUri;",
        "    const char *metaHex;",
        "    const char *mainHex;",
        "    const char *auxHex;",
        "    const Field *mainFields;",
        "    size_t mainFieldCount;",
        "    const Field *auxFields;",
        "    size_t auxFieldCount;",
        "    // Fields present in the raw region that this firmware does not map.",
        "    int unmappedMainFields;",
        "    int unmappedAuxFields;",
        "};",
        "",
    ]

    entries = []
    for name in names:
        info = load_yaml(FIXTURES / f"{name}_info.yaml")
        regions = info["regions"]
        root = info["root"]
        raw = info.get("raw_data", {})

        main_fields, main_recognised = build_fields(
            info["data"].get("main"), main_keys, FIELD_KINDS, enums)
        uuid_fields, uuid_recognised = build_uuid_fields(raw.get("main"))
        main_fields = sorted(main_fields + uuid_fields, key=lambda f: f["key"])
        main_recognised += uuid_recognised
        aux_fields, aux_recognised = build_fields(
            info["data"].get("aux"), aux_keys, AUX_FIELD_KINDS, enums)

        main_total = count_cbor_map_keys(bytes.fromhex(raw["main"])) if raw.get("main") else 0
        aux_total = count_cbor_map_keys(bytes.fromhex(raw["aux"])) if raw.get("aux") else 0

        out.append(f"inline constexpr Field k{name}Main[] = {{")
        for field in main_fields:
            out.append(f"    {{{c_string(field['name'])}, {field['key']}, "
                       f"Kind::{field['kind']}, {field['int']}, "
                       f"{field['float']!r}, {c_string(field['text'])}}},")
        out.append("};")
        out.append(f"inline constexpr Field k{name}Aux[] = {{")
        for field in aux_fields:
            out.append(f"    {{{c_string(field['name'])}, {field['key']}, "
                       f"Kind::{field['kind']}, {field['int']}, "
                       f"{field['float']!r}, {c_string(field['text'])}}},")
        out.append("};")
        out.append("")

        aux_region = regions.get("aux")
        entries.append({
            "name": name,
            "main_len": len(main_fields),
            "aux_len": len(aux_fields),
            "text": "    {" + ", ".join([
                c_string(name),
                c_string(f"fixtures/{name}_data.bin"),
                str(root.get("cc_capacity", 0)),
                str(root["data_size"]),
                str(regions["meta"]["absolute_offset"]),
                str(root["payload_size"]),
                "{%d, %d, %d}" % (regions["meta"]["payload_offset"],
                                  regions["meta"]["absolute_offset"],
                                  regions["meta"]["size"]),
                "{%d, %d, %d}" % (regions["main"]["payload_offset"],
                                  regions["main"]["absolute_offset"],
                                  regions["main"]["size"]),
                ("{%d, %d, %d}" % (aux_region["payload_offset"],
                                   aux_region["absolute_offset"],
                                   aux_region["size"])) if aux_region else "{0, 0, 0}",
                "true" if aux_region else "false",
                "true" if info.get("uri") else "false",
                c_string(raw.get("meta", "")),
                c_string(raw.get("main", "")),
                c_string(raw.get("aux", "")),
                f"k{name}Main",
                f"sizeof(k{name}Main) / sizeof(Field)" if main_fields else "0",
                f"k{name}Aux",
                f"sizeof(k{name}Aux) / sizeof(Field)" if aux_fields else "0",
                str(main_total - main_recognised),
                str(aux_total - aux_recognised),
            ]) + "},",
        })

    out.append("inline constexpr Fixture kFixtures[] = {")
    for entry in entries:
        out.append(entry["text"])
    out.append("};")
    out.append("inline constexpr size_t kFixtureCount = "
               "sizeof(kFixtures) / sizeof(Fixture);")
    out.append("")
    out.append("}  // namespace fixtures")
    out.append("")

    OUT.write_text("\n".join(out))
    print(f"wrote {OUT.relative_to(ROOT)}: {len(entries)} fixtures")


if __name__ == "__main__":
    main()

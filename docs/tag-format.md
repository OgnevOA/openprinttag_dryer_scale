# Tag format notes

Implementation notes on the codec in
`firmware/OpenPrintTagDryer/src/openprinttag/`. The authority is the
[OpenPrintTag specification](https://github.com/OpenPrintTag/openprinttag-specification)
(MIT); this file records what the firmware does with it and why, which is the
part the spec cannot tell you.

The codec is deliberately free of Arduino includes, so it compiles on a desktop
and is tested there against the specification's own fixtures before any hardware
is involved.

## On the tag

An ICODE SLIX2 has 79 blocks of 4 bytes: 316 bytes of user memory, addressed a
block at a time. The layout inside it is NFC Forum Type 5:

```
+--------------------------------------------------+
| CC, 4 B          capability container            |
| NDEF TLV         type 0x03, length, then value   |
|   NDEF record    MIME application/vnd.openprinttag
|     payload      CBOR: meta map, main, aux       |
| terminator TLV   type 0xFE                       |
+--------------------------------------------------+
```

Three details that matter when writing rather than reading:

- The CC expresses memory size in 8-byte units, so the image is rounded down to a
  multiple of 8 bytes.
- Two length encodings exist for the TLV, one byte and three. Both are read; the
  writer picks whichever the payload needs.
- The payload is padded to fill the tag, as the spec recommends. That is what
  makes later aux updates safe: nothing ever has to move, so no update can
  disturb the main region.

## The payload

CBOR, in three parts.

**Meta** is a map declaring where the other two are:

| Key | Meaning |
| --- | --- |
| 0 | main region offset |
| 1 | main region size |
| 2 | aux region offset |
| 3 | aux region size |

**Main** is the static description of the spool, written once at provisioning.
The fields this firmware reads and writes:

| Key | Field | Used for |
| --- | --- | --- |
| 5 | brand_specific_instance_id | The Prusament spool ID from the QR code |
| 8 | material_class | |
| 9 | material_type | The material sensor, and the enum name |
| 10 | material_name | `PETG Prusa Orange`, the way Prusa's own tags read |
| 11 | brand_name | `Prusament` |
| 13 | write_protection | Written as none |
| 16 | nominal_netto_full_weight | 1000 g, the label figure |
| 17 | actual_netto_full_weight | The measured figure; what remaining is computed against |
| 18 | empty_container_weight | Subtracted from gross |
| 19 | primary_color | RGBA |
| 29 | density | Remaining length, when the length fields are absent |
| 30 | filament_diameter | Same |
| 34 / 35 | min / max print temperature | Reported to Home Assistant |
| 37 / 38 | min / max bed temperature | From the material preset |
| 52 | material_abbreviation | Fallback when material_type is absent |
| 53 / 54 | nominal / actual full length | Remaining length |
| 57 | drying_temperature | What the HA automation acts on |
| 58 | drying_time | Same |

**Aux** is the mutable part, 32 bytes, aligned to a 4-byte block boundary:

| Key | Field | Notes |
| --- | --- | --- |
| 0 | consumed_weight | Written from the scale |
| 1 | workgroup | Which device owns this aux data |
| 4 | storage_location | Optional, 8 characters |

It is encoded as an indefinite-length map, which is what allows a value to change
length without moving everything after it.

## Update rules the firmware honours

Straight from the spec, and each one is a way to corrupt somebody's tag if
ignored:

1. Never write outside the aux region during normal operation.
2. Never move a region or rewrite the meta map after provisioning.
3. Preserve keys you do not understand. A future field written by another tool
   survives a `consumed_weight` update from this one.
4. Write only the 4-byte blocks that differ.
5. On a workgroup mismatch, offer a wipe rather than silently trusting or
   overwriting the aux data.

Rule 4 is what keeps the write count survivable. `NfcManager` builds the new aux
region, diffs it against the blocks it read, and writes the difference — typically
one or two blocks against the tag's 100,000-cycle endurance.

Rule 3 has a limit worth stating plainly, because it is invisible until it costs
you something. The aux region is re-encoded from the bytes already on the tag, so
unknown fields there survive by construction. The main region is not: provisioning
builds it from the form and the preset, which means every field the decoder does
not model is gone. On a factory Prusament tag that is `certifications`,
`primary_color_lab` and `primary_color_ral` — nothing this device needs, and not
its to delete either.

So a tag that needs a *layout* change rather than new content gets a different
operation. **Add an auxiliary region** on the Tag page copies the main region byte
for byte into a rebuilt image and only changes the meta map and the region
boundaries, which is the one way to keep fields the encoder cannot express. It
exists for tags written without an aux region: perfectly readable, and otherwise
permanently unwritable. The leading URI record is dropped in the process, since
its bytes are what the new region is made of.

## The CBOR subset

Only what the spec uses: unsigned and negative integers, byte and text strings,
arrays, maps, half, single and double floats, booleans, null, and
indefinite-length maps and arrays. No tags, no bignums, no streaming strings.

Decoding is written to survive a tag that is wrong rather than merely
unrecognised. Truncated payloads, lengths that overrun the buffer, absurd nesting
and garbage in place of a map all produce a decode failure, never a read past the
end. That is not a matter of taste: the input is 316 bytes of anything, arriving
over the air.

## Identity UUIDs

Spec section 3.2 lets a writer either store the four identity UUIDs or leave
them to be derived, and Prusa's tags leave them out. The derivations were checked
against the specification's own fixture and against a factory Prusament spool,
and both reproduce exactly:

| UUID | UUIDv5 of | Namespace |
| --- | --- | --- |
| `brand_uuid` | `brand_name` | `5269dfb7-1559-440a-85be-aba5f3eff2d2` |
| `material_uuid` | `brand_uuid` + `material_name` | `616fc86d-7d99-4953-96c7-46d2836b9be9` |
| `package_uuid` | `brand_uuid` + `gtin` | `6f7d485e-db8d-4979-904e-a231cd6602b2` |
| `instance_uuid` | the 8-byte NFC UID | `31062f81-b5bd-4f86-a5f8-46367e841508` |

The device neither derives nor writes them — it has no use for a UUID and
SHA-1 is not worth the flash. What matters is the consequence for provisioning:
`material_uuid` is a hash of `material_name`, so the preset that writes
`"PLA Prusa" + "Galaxy Black"` has to reproduce Prusa's `"PLA Prusa Galaxy
Black"` character for character, or every other tool will treat the spool as a
different material. There is a test pinned to exactly that string.

A UUID another tool stored explicitly is decoded and carried, so a rewrite here
does not silently replace a deliberate identity with a derived one.

## What is deliberately not written
- **Write protection.** These are your own tags, and `PROTECT PAGE` would only
  make correcting a typo impossible.
- **SLA and resin fields.** Nothing here can measure them.
- **A Spoolman spool id.** It would make the lookup trivial — no searching at all
  — and other people's scanners do exactly that. But a spool id only means
  anything inside one Spoolman database, and a tag is meant to be readable by a
  phone, a printer and somebody else's dryer. So the link is stored on the
  Spoolman side, keyed on the tag's hardware UID, which every reader can produce
  and nothing has to agree about. See [spoolman.md](spoolman.md).

## Testing

```bash
cd tests && make test
```

Two binaries. `opt_tests` checks the codec against the specification's fixtures —
`tests/fixtures/0*_data.bin` paired with the expectations generated from the
matching `0*_input.yaml`, plus `sample_tag.bin` — and round-trips the encoder
against them. `logic_tests` covers everything above the codec.

The fixtures and the enum name tables are generated rather than transcribed:

```bash
python3 tools/gen_enums.py                 # -> src/openprinttag/opt_enums.h
python3 tools/gen_fixture_expectations.py  # -> tests/fixture_expectations.h
```

Two things the tests deliberately include, because they are what a real tag does:

- **Mutation robustness.** Fixtures are corrupted byte by byte and fed back in.
  Every mutation must produce either a clean decode or a clean failure.
- **Aux round-trips.** A `consumed_weight` update is applied to a region that
  contains a key the firmware does not know, then re-decoded: the unknown key's
  bytes must survive verbatim, and the block diff must come to no more than two
  blocks. The encoder tests separately assert that a rebuilt main region is
  byte-identical to the fixture's.

Since there is no golden reference for our own encoder, acceptance is: byte-level
agreement with the spec's fixtures wherever the encoding is deterministic, plus
reading a tag we wrote with the Prusa app and a third-party NFC app
([validation test 4](validation.md#4-provisioning-and-read-back)).

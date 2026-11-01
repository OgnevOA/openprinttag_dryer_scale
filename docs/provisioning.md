# Provisioning a spool

Prusament spools made before the October 2025 redesign carry a QR code and no
NFC tag. Provisioning is the one-time job of putting the data that QR code points
at onto a blank OpenPrintTag, so that everything afterwards — this device, a
phone, a printer that reads tags — gets it from the spool itself.

You need: the spool, a blank OpenPrintTag, and the spool's page at
`prusament.com/spool/?spoolId=<id>` open on a phone.

Use the official Ø90 mm tags rather than generic ICODE SLIX2 stickers. Two
reasons, both practical: the spec's reader requirements assume that antenna
diameter, and Prusa rates the adhesive tag to 80 °C, which matters when it spends
its life inside a 70 °C dryer.

## If the Prusa app already wrote the tag

Prusa's phone app writes the same standard, so a tag it provisioned is a valid
tag. It is not a finished one for this device, for one reason that has nothing to
do with the app: **it copies the spool weight off the spool's page, which does not
include the adapter.** The app cannot know about a 31 g part you added, so the tag
claims a 186 g tare where the platform sees 217 g, and every remaining weight, the
`consumed_weight` written back, and everything sent to Spoolman inherits the
error.

Fix it with **Rewrite the empty spool weight** on the Tag page, which changes that
one field and leaves the rest of the region alone. **Do not use Provision for
this.** An app-written tag holds a good deal the form cannot ask for — a GTIN, a
manufactured date, nominal weights and lengths beside the actual ones, the
container's measurements, chamber temperatures, material tags, a stored instance
UUID — and provisioning over it would drop all of that to correct 31 g. See
[Fixing a tag](#fixing-a-tag).

So the app is not the long way round after all, as long as the tare gets
corrected: it fetches more about the spool than the portal's form knows how to
ask. Provisioning from the portal is still the only option for a spool with no
QR code to look up, and it is the quicker path when you are doing several.

Either way, go to **Tag** in the portal and check two rows:

- **Aux region.** If it says *none, so nothing can be written*, the app left no
  room for the region this device writes `consumed_weight` into, and consumption
  will be reported from the scale but never stored on the spool. The page then
  offers **Add an auxiliary region**, which rebuilds the tag around the main
  region it already carries — byte for byte, so nothing the app wrote is
  reinterpreted or lost. Do this one first: correcting the tare needs an
  auxiliary region to exist, since the rewrite carries that region across rather
  than creating it.
- **Unknown fields carried over.** Anything above zero is a field the app wrote
  that this firmware does not model. That is fine and by design: they are
  preserved byte-for-byte through every write-back. It is worth knowing the count
  before the first write so you can confirm afterwards that it has not moved.

While you are on that page, use **Download these bytes**. A tag written by
somebody else's implementation is the only independent check this project's
decoder has, so it is worth keeping in `tests/fixtures/`, and it can be read
back field by field on the desktop:

```
make -C tests dump_tag
tests/dump_tag tag_E0_04_01_50_XX_XX_XX_XX.bin
```

That runs the firmware's codec, not a second implementation of it, so agreement
means the device will read the spool the same way. Fixture `01_data.bin` is the
specification's own example and happens to be PLA Prusa Galaxy Black, which
makes it a useful thing to dump alongside for comparison.

## 1 Stick the tag on

On the spool flange, concentric with the spool, clear of any metal. Concentric is
not cosmetic: the tag's antenna is a ring trace at Ø74-79 mm, the reader coil at
the lid is aimed at that ring, and a tag stuck off-centre moves the ring out from
under the coil.

Then put the spool in the dryer and close the lid.

## 2 Read the numbers off the spool's page

Scan the QR code. The page gives the values Prusa measured for that individual
spool:

| The page says | Goes into | Tag field |
| --- | --- | --- |
| Filament weight, e.g. 1044 g | Filament weight | `actual_netto_full_weight` |
| Spool weight, e.g. 186 g | Spool weight, **plus the adapter** | `empty_container_weight` |
| Length, e.g. 343 m | Length | `actual_full_length` |
| The ID in the URL, e.g. `7ab2acb509` | Spool ID from the QR code | `brand_specific_instance_id` |

This per-spool measurement is the reason being Prusament-only is a help rather
than a limitation. The reference weight on the tag is accurate to the gram, so
the scale's resolution is the limit on knowing how much filament is left, not the
reference it is measured against.

## 3 Fill in the form

**New spool** on the portal.

- **Material** picks up everything constant per material: density, diameter,
  print and bed temperatures, drying temperature and time. The list marks the
  materials that want a hotter dryer than the Space Pi's usual setting.
- **Colour** comes from a preset list, or from the box beside it for a colour the
  list has not caught up with — Prusament colours arrive faster than any table
  tracks them. The colour becomes part of the material name on the tag the way
  Prusa's own tags read it — brand `Prusament`, material name `PETG Prusa
  Orange` — as well as being written as RGBA.

  Spell a typed-in colour exactly as Prusa spells it. `material_name` is
  `<material> <colour>`, and `material_uuid` is derived from it by UUIDv5, so
  "Linden light" and "Linden Light" are different materials to every other tool
  that reads the tag. A typed-in colour also needs a hex value, since there is no
  list to take one from; provisioning refuses rather than defaulting to grey and
  writing a confidently wrong colour. Sample it off the spool if Prusa publishes
  none — Woodfill is one where they do not.
- **Filament weight** and **spool weight** are the two numbers everything else is
  derived from. Take the filament weight straight off the page.
- **Spool weight is the spool's page figure plus the adapter**, because this field
  is the whole tare: everything that stays on the platform once the filament is
  gone. The printed adapter is bonded to the spool and never comes off, so 186 g
  of spool and a 31 g adapter make 217 g. The placeholder shows that sum, using
  `OPT_ADAPTER_GRAMS` from `config.h` — reweigh yours if you reprinted it.
  Forgetting the adapter does not fail quietly; see step 4.
- **Length** is optional. Without it the remaining length is computed from
  density and diameter, which is close but not spool-specific.
- **Also write a link to the spool's page** adds a second NDEF record with the
  URL, so tapping the spool with a phone opens its page. It costs tag memory and
  is off by default.

## 4 Let the scale check your typing

Before writing anything, the device compares what you entered against what it
weighs:

```
gross - dryer_tare  ≈  filament weight + spool weight
```

Agreement within 20 g and it writes. Beyond that it refuses and says by how much.
The tolerance is set where it is deliberately: the cell is good to a gram or two
and the tare to a few more, so 20 g clears real measurement error while still
catching a transposed digit.

It also catches a forgotten adapter, since 31 g does not fit in 20 g of slack. So
a refusal reading roughly *31 g heavier than you entered* means you copied the
page's spool weight without adding the adapter, rather than anything being wrong
with the scale or the tare.

A refusal means one of these, roughly in order of likelihood:

1. A typo in one of the three numbers.
2. The wrong spool ID — an easy mistake when several spools of the same material
   are on the shelf.
3. The spool is not actually sealed. A spool you have printed 200 g from will
   fail this check, correctly.
4. The dryer tare is stale, because the dryer was moved. Re-tare
   ([calibration.md](calibration.md#dryer-tare)) and try again.

**Write even if the scale disagrees** exists for case 3, where you know why the
numbers differ. Using it to get past case 1 writes a wrong tag that everything
downstream will believe.

The check is skipped, with a note saying so, when the scale is not calibrated,
the dryer has not been tared, or the weight has not settled yet. It is worth
having the scale working first: this check is the only thing standing between a
typo and a spool that quietly reports the wrong material for the rest of its life.

## 5 What gets written

The whole tag image, not just a data field: capability container, NDEF TLV, one
NDEF record of MIME type `application/vnd.openprinttag`, and the CBOR payload
inside it.

- The payload is padded out to fill the tag's 316 bytes, as the specification
  recommends, so the region positions never have to move later.
- The **auxiliary region** is 32 bytes, block-aligned, and holds `consumed_weight`
  (starting at zero) and this device's workgroup. It is the only part of the tag
  touched afterwards.
- **UUID fields are left out** on purpose. `instance_uuid` is derivable from the
  tag UID, and the brand, material and package UUIDs are derivable by UUIDv5
  from the brand and material names, so writing them would spend scarce bytes on
  information that is already implied.
- **Write protection is set to none.** These are your own tags; a typo should be
  correctable by rewriting rather than being permanent.

After writing, the device reads the tag back and decodes it. A tag that does not
decode is reported as a failure, not quietly accepted.

More on the format: [tag-format.md](tag-format.md).

## 6 Verify it independently

The device verifying its own write only proves the codec agrees with itself. Two
minutes with a phone proves it agrees with everybody else:

1. The **Prusa mobile app** should show the material and the weights.
2. A generic NFC app — NFC Tools, or NXP TagInfo — should report an NDEF message
   whose record type is exactly `application/vnd.openprinttag`.

That is [validation test 4](validation.md#4-provisioning-and-read-back), and it is
worth doing for the first tag you write even if you skip it later.

## Afterwards

From then on the device only ever touches the auxiliary region, writing the
measured `consumed_weight` back when the weight has settled, the change is worth
recording, and enough time has passed. Only the 4-byte blocks that actually
differ are written, so a spool that lives in the dryer for months spends a
bounded and small number of the tag's 100,000 write cycles.

## Fixing a tag

**A wrong empty spool weight.** Use **Rewrite the empty spool weight** on the Tag
page rather than provisioning again. It decodes the tag, changes that one field,
re-encodes and writes only the blocks that differ, so everything else survives
unchanged — including fields the provisioning form cannot ask for.

That matters most on a tag the Prusa app wrote, which carries far more than the
form offers: a GTIN, a manufactured date, nominal as well as actual weights and
lengths, the container's four measurements, chamber temperatures, material tags
and a stored instance UUID. **Provisioning over it would silently drop every one
of them.** The commonest reason to need this is a printed adapter bonded to the
spool after the tag was written, which is 31 g of tare the app had no way to know
about.

It refuses rather than proceeding in two cases, both of which mean rewriting
would lose something the firmware cannot reproduce: a main region carrying a field
this firmware does not model, and a URI record whose address cannot be rebuilt.
It also refuses a tare heavier than what is on the platform, which is the digit
slip — 1860 for 186 — that a plausibility bound alone would let through.

The consumed weight on the tag is left alone by the rewrite even though the new
tare changes what it should be. It corrects itself at the next settled reading
through the ordinary write-back path. Spoolman's copy of the spool weight is
separate and has to be corrected there by hand.

**A typo in the material, or the filament weight.** Provision it again with the
right numbers. Nothing is locked, so the main region is simply rewritten — but
read the paragraph above first if the tag was not written by this device.

**A tag from someone else's device.** The workgroup on the tag will not match
yours, and the device will not write to a foreign workgroup. **Wipe auxiliary
region** resets it, which is offered rather than done silently, because the aux
data belongs to whoever wrote it.

**A tag that has stopped reading.** Check it against the reader on the bench
before blaming the bracket. Adhesive tags do fail — a crease across the antenna
ring is enough — and a new tag costs 55 cents.

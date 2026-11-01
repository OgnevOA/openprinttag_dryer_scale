# Validation

Eight tests, in order, the last of which only applies if you run Spoolman. Each
one depends on the ones before it, so a failure that
is skipped rather than fixed will reappear later wearing a disguise: a bad tare
looks like thermal drift, and thermal drift looks like a spool that is losing
filament on its own.

Everything here runs from the device's own web portal at
`http://openprinttag-dryer.local/`. Nothing needs a bench supply, a logic
analyser or a NanoVNA.

| # | Test | Proves | Time |
| --- | --- | --- | --- |
| 1 | [Bench self-test](#1-bench-self-test) | Both radios and the converter answer | 2 min |
| 2 | [Scale calibration and linearity](#2-scale-calibration-and-linearity) | Grams are grams | 20 min |
| 3 | [Lid read reliability](#3-lid-read-reliability) | The bracket is in the right place | 15 min |
| 4 | [Provisioning and read-back](#4-provisioning-and-read-back) | A written tag is a valid tag | 20 min |
| 5 | [Tare cross-check](#5-tare-cross-check) | The weight maths agrees with the tag | 10 min |
| 6 | [Thermal drift run](#6-thermal-drift-run) | Compensation over a real dry cycle | 6-8 h |
| 7 | [End-to-end write-back](#7-end-to-end-write-back) | The whole loop, unattended | 24 h |
| 8 | [Spoolman](#8-spoolman-if-you-run-it) | A good weight lands, a bad one cannot | 15 min |

Record the results in [the log at the end](#results-log). The numbers matter
later: when the reading drifts in six months, the only way to know whether
something changed is to have written down what it was.

## 1 Bench self-test

Before anything is mounted. Reader on the bench next to the carrier, load cell
connected, nothing on the platform.

Open **Self-test**. Everything on that page comes from probing the hardware at
that moment, not from a cached boot result.

```
PN5180 reader
  version: PN5180 fw 4.1, eeprom 1.0
  RF field: on
  no tag in the field
HX711 scale
  converter: responding
  raw counts: 8412334
  span: 430.000 counts/g (DEFAULT)
  zero: 0 counts
  dryer tare: 0.0 g
  gross now: 19563.6 g, unsettled
  temperature sensor: I2C 0x48, 24.31 C
  drift coefficient: 0.200 g/C around 25.0 C
```

**Pass:** a reader version string, converter responding, raw counts changing when
you press on the cell, temperature sensor found. The weights are nonsense at this
point and are supposed to be: nothing is calibrated yet.

**If the reader is not online**, it is nearly always power or BUSY. The PN5180
needs 5 V at up to 150 mA; a XIAO fed from a laptop USB port through a long
cable will brown out the moment the field starts. Check 5 V at the module's own
pins while the field is on, not at the carrier.

**If the converter is not responding**, the driver never saw DOUT go low. That
is either the DOUT/SCK pair swapped or no excitation, which you can tell apart
with a meter: E+ to E- should read the supply, and the bridge should be about
1 kΩ across either pair.

**If no temperature sensor is found**, the drift run in test 6 becomes
guesswork. It is worth fixing now: the driver probes 0x48-0x4B, so an LM75 or
TMP102 with its address pins strapped anywhere in that range will be found, and
a DS18B20 is looked for after that. A DS18B20 that is not found is nearly always
a missing 4.7 kΩ pull-up to 3.3 V on its data line rather than a dead part.

## 2 Scale calibration and linearity

Mount the sandwich under the dryer with the isolator feet fitted, but keep the
dryer cold and empty. Then, on **Scale**:

1. **Zero empty platform** with nothing on it. This is the cell's own offset,
   not the dryer's weight.
2. Put a known mass on the platform, enter it, and **Calibrate span**. A
   2 kg mass is a good choice: heavy enough that a gram of error is a small
   fraction of it, light enough to handle. Two 1 kg spools in their bags work
   if you have nothing else, though their printed weight is nominal.
3. Put the empty dryer on with the lid closed and press **Tare dryer**.

Now check linearity, which calibration alone does not prove. With the dryer on
the platform, add masses one at a time and read the gross weight:

| Added | Expected | Tolerance |
| --- | --- | --- |
| nothing | 0 g | ±2 g |
| 500 g | 500 g | ±3 g |
| 1000 g | 1000 g | ±4 g |
| 2000 g | 2000 g | ±6 g |

**Pass:** every point inside tolerance, and the reading returns to within 2 g of
zero when everything is removed.

**If it comes back low after unloading**, the platform is fouling something —
usually the overload stop set too close, or a cable pulling on the platform.
The stop should only make contact under a load the cell should not see at all.

**If the error grows with load rather than staying put**, the span is wrong
rather than the zero: recalibrate with a heavier reference, and check the mass
itself. Kitchen scales are frequently out by 1%.

## 3 Lid read reliability

This is the test the whole mechanical design exists to pass, and the one most
likely to need iterating. Put a provisioned spool in the dryer — or an
unprovisioned blank tag stuck to a spool flange, since inventory does not care
what the tag contains — and close the lid.

On **Self-test**, run the **read test** with 20 attempts.

```
Read test, 20 attempts
  found the tag 20 of 20 times (100%)
  inventory time: 11 ms best, 14 ms average, 31 ms worst
  full image read: 316 B in 210 ms, decoded
```

**Pass:** 20 of 20, and a full image read that decodes. Anything less is not
"mostly working": a spool that reads 80% of the time will fail a write halfway
through, and a half-written aux region is worse than an unwritten one.

Iterate in this order, because they are in order of effect:

1. **Concentricity.** The tag's antenna is a Ø74-79 mm ring. The reader coil
   couples to that ring, so it wants to sit over the ring, not over the middle
   of the spool. Move the bracket sideways in 5 mm steps and re-run.
2. **Distance.** Every millimetre of the lid counts against you. The bracket is
   designed to hold the module against the inside of the lid; check nothing —
   a cable, a printed boss, a screw head — is holding it off.
3. **Rotation.** With the spool turned 90°, re-run. A rate that changes with
   rotation means the coil is near the edge of the ring rather than over it.
4. **Metal.** A steel spool hub or a magnet in the dryer lid will detune the
   coil. The tag also needs to be clear of the spool's own metal, if it has any.

Note the average inventory time. It goes into the results log because it is the
early warning: a rate still at 100% but with the average time doubled means the
coupling has degraded and the margin is nearly gone.

**Only if all four steps fail** is the matched Ø75 mm antenna from the risk list
worth taking on. It needs a NanoVNA and is a phase of its own.

## 4 Provisioning and read-back

Take one spool whose `prusament.com` page you have open, and one blank tag.

1. Stick the tag on the spool flange, concentric and clear of metal.
2. In **New spool**, choose the material, enter the spool ID, and copy the
   filament weight, spool weight and length off the page.
3. Submit. The device cross-checks the entry against the scale before writing:
   `gross - dryer_tare` should equal filament plus spool weight.

**Pass:** the write succeeds, the read-back decodes, and the **Tag** page shows
the material, colour and weights you entered.

**If the cross-check complains** by more than a few grams, believe it. In order
of likelihood: a typo in one of the three numbers, the wrong spool ID, filament
already used from a spool you thought was sealed, or a tare captured with
something still in the dryer.

Then the independent check, which is the point of this test — the device
verifying its own writes only proves the codec is self-consistent:

4. Read the tag with the **Prusa mobile app**. It should show the material and
   the weights.
5. Read it with a generic NFC app (NFC Tools, or NXP TagInfo). It should report
   an NDEF message with a MIME record of type `application/vnd.openprinttag`.

**If the Prusa app wrote the tag**, run this test backwards, which is the
stronger direction: the tag is the known-good artefact and the device is what is
under test. Put the spool in the dryer, open **Tag**, and compare every field
against the app's own view of it — then check the **Aux region** row, because a
tag written without one has nowhere for `consumed_weight` to go and the page's
**Add an auxiliary region** is the fix that keeps the factory data intact. Save
the image with **Download these bytes** first, and dump it
as described in [provisioning.md](provisioning.md). A field that reads wrong here
is a decoder bug, and it is worth stopping to fix: the codec is in
`firmware/OpenPrintTagDryer/src/openprinttag/` and the fixture you just captured
belongs in `tests/fixtures/`.

**Pass:** both apps read it, and the MIME type is exactly right. A tag that this
device can read but nothing else can is a tag that only appears to work.

## 5 Tare cross-check

The dryer tare is the one constant the whole weight chain rests on, and a sealed
spool gives a second, independent way to measure it. With a sealed spool in the
dryer — `consumed_weight` zero or absent — the tag's own reference weights say
what the dryer must weigh:

```
dryer_tare = gross - (actual_netto_full_weight + empty_container_weight)
```

The **Scale** page's cross-checks card shows what the dryer must weigh and how
far the stored tare is from it.

**Pass:** the two agree within 5 g. That is the acceptance test for the entire
chain — cell, converter, calibration, tare and tag data — in a single number.

**If they disagree**, work out which one to trust before adopting either. A
sealed spool that has been opened, or a tag provisioned from a mistyped weight,
will produce a confident and wrong implied tare. If the entry in test 4 checked
out and the linearity in test 2 held, the implied value is the better one, and
**Use the implied tare** takes it.

## 6 Thermal drift run

The hot dryer sits on the load cell, and load cells care about temperature. This
run measures how much, over the range the device will actually see.

Requirements: the span calibrated (test 2), a temperature sensor found (test 1),
and a load that will not change. Use a sealed spool, or a plain 1 kg mass — not
a wet spool, which loses grams of water and would be indistinguishable from
drift.

1. Let everything sit until the dryer is at room temperature. The cold end of
   the run is the part that cannot be reproduced later.
2. On **Self-test**, start the drift log. The default 30 s interval covers six
   hours, which is a full cycle.
3. Run the dryer at the temperature you actually dry at — 70 °C for the awkward
   materials — for a full cycle, then let it cool with the log still running.
   The cooling leg is not optional: it is what separates temperature drift from
   mechanical hysteresis.
4. Stop the log.

The page shows the fit:

```
Fit    0.214 g/C over 31.4 C, R² 0.981
```

**Pass:** R² above 0.8 over at least 20 °C of span. Then **Apply fit**, which
stores the coefficient and the reference temperature. The device refuses to
apply a fit over less than 5 °C or with R² below 0.5, because a slope through
noise fits anything.

For the analysis the device cannot do, download `drift.csv` and run:

```bash
python3 tools/fit_drift.py drift.csv
```

That separates the heating leg from the cooling leg, reports the hysteresis
between them, and prints the scatter so a run spoiled by somebody leaning on the
dryer is visible rather than averaged in.

**If the legs disagree** by much, the problem is mechanical, not thermal, and no
coefficient will fix it: something in the mount moves as it warms. Check the
screws, check that the platform is not touching the dryer shell anywhere except
through the isolator feet, and check that no cable stiffens across the gap.

**If the coefficient is large** — say more than a few grams per degree — the cell
itself is being heated rather than the case around it. That is what the isolator
feet exist to prevent; fit them before compensating for their absence.

After applying, re-run a short cycle and watch the remaining weight. Compensated
correctly, it should sit still while the temperature climbs. That is the real
acceptance test: the coefficient is a means, and a steady reading is the end.

## 7 End-to-end write-back

Everything together, unattended, on a spool that is actually being printed from.

1. Confirm auto write-back is on and note the tag's current `consumed_weight` on
   the **Tag** page.
2. Print until at least 20 g has gone — comfortably past the 5 g write
   threshold.
3. Put the spool back, close the lid, and leave it to settle.

The device writes when the weight has been stable for the configured window, the
delta exceeds the threshold, the minimum interval has passed, and the workgroup
matches. Then:

**Pass:** the **Tag** page shows the new `consumed_weight` matching the scale's
figure within a few grams, the aux region write touched only a couple of blocks,
and the main region is byte-identical to what test 4 wrote. The Prusa app still
reads the tag.

Leave it a week in normal use and check again. What is being tested over that
week is the write count: aux writes should be occasional, not constant. If the
last-write timestamp moves every few minutes, the threshold is too low for how
noisy your scale is, or the stability window is too short — either way the tag's
100k endurance is being spent on nothing.

## 8 Spoolman, if you run it

Only if the Spoolman address is configured. The point of this test is not that
the number arrives — that part is one HTTP call — but that a bad number does not.

1. On the **Tag** page's Spoolman card, link a tagged spool to its Spoolman
   entry, and note what **Tags identified by** says. Check in Spoolman that the
   spool now carries the UID, either on its tag list or as `nfc_uid` under its
   extra fields.
2. Press **Send the weight to Spoolman now**. In Spoolman, the spool's remaining
   weight should match what the **Status** page shows as remaining, within a few
   grams. If it is out by roughly the weight of an empty spool, Spoolman's *empty
   spool weight* for that entry disagrees with the tag's `empty_container_weight`
   — fix it in Spoolman, since it is the number `measure` subtracts.
3. Now try to break it. Note the stored dryer tare, set it to zero on the
   **Scale** page, and press the button again.

**Pass:** step 3 sends nothing and the card explains that the dryer has not been
tared. Spoolman's figure for that spool is unchanged — in particular its full
weight is unchanged, which is what a reading with the dryer's 3 kg in it would
have overwritten. Restore the tare afterwards and confirm a normal push works
again.

4. Unplug Spoolman, or stop the container, and leave a spool in the dryer for ten
   minutes.

**Pass:** the Spoolman card shows *cannot reach Spoolman*, and everything else
carries on — weights update in Home Assistant, and tag write-back still happens.
Spoolman being down must never be able to stop the device working.

## Results log

Copy this, fill it in, keep it with the device.

```
Device                      openprinttag-dryer, firmware ____
Date                        ____

1 self-test        reader firmware ____   temp sensor 0x__
2 calibration      ____ counts/g   zero return ____ g
                   500 g: ____   1000 g: ____   2000 g: ____
3 lid reads        ____/20 (____%)   average ____ ms
                   bracket position: ____
4 provisioning     spool ID ____   cross-check error ____ g
                   Prusa app: ____   generic NFC app: ____
5 tare cross-check stored ____ g   implied ____ g   error ____ g
6 drift run        ____ g/C over ____ C, R² ____
                   hysteresis ____ g   applied: ____
7 write-back       tag consumed ____ g   scale ____ g   delta ____ g
                   writes in the first week: ____
8 spoolman         identified by ____   remaining there ____ g / here ____ g
                   untared push refused: ____   survives Spoolman down: ____
```

## What "good" looks like

For reference, a device that has passed all of them:

| Measure | Expected |
| --- | --- |
| Lid read rate | 20/20, 10-20 ms average inventory |
| Full image read | 316 B in under 400 ms |
| Weight repeatability | ±2 g cold, ±3 g through a dry cycle |
| Tare cross-check | within 5 g of the sealed-spool figure |
| Drift coefficient | a few tenths of a gram per degree, R² above 0.9 |
| Aux writes | a handful per spool, not per hour |

A gram or two is the honest floor here. A 5 kg cell reading a 1.1 kg dryer plus
a 1.2 kg spool is using a quarter of its range, and the last gram of that is
being asked for through a heated enclosure. Remaining filament good to a few
grams is a couple of metres of PLA, which is far better than the alternative of
guessing from print time.
